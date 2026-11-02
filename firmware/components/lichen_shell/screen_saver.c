// The clock screensaver and the pocket lock (#75). Both are covers (shell_set_cover): normal screens shown in front of
// the screen stack without being in it, so whatever opens below them stays put and a screenshot sees them.
// The saver is screen_idle's dim stage, in the style Settings > Display picks (#119): a Swiss railway style station
// clock (saver_station.c, the default) or a seven-segment clock (saver_layout.c) drifting a little every minute; both
// with the date and the unread count, the status bar hidden meanwhile. The lock engages with the saver or the
// dark screen when Settings > Display has it on; its screen swallows every key (a raw listener) until Esc and
// Backspace are held together (lock_combo.c). It is a pocket lock, not security: no PIN, and an update, power off or a
// battery warning lifts it. Toasts still show over both (top layer); the shape keys do nothing while locked.
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fonts.h"
#include "fw_console.h"
#include "idle_policy.h"
#include "lichen_clock.h"
#include "lichen_notify.h"
#include "lichen_ui.h"
#include "lock_combo.h"
#include "raw_keys.h"
#include "saver_layout.h"
#include "saver_station.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static char const TAG[] = "saver";

#define SAVER_TICK_MS 1000
#define LOCK_TICK_MS  50
#define DRIFT_MS      60000 // the saver moves once a minute
#define EDGE          24    // the drifting clock keeps this far from the panel's edge

// A seven-segment HH:MM with the date and the unread count under it
typedef struct {
    lv_obj_t *box;
    lv_obj_t *seg[4][SEG7_COUNT];
    lv_obj_t *colon[2];
    lv_obj_t *date;
    lv_obj_t *notes;
    char      shown[8]; // the digits and colour last drawn
} big_clock_t;

typedef struct {
    int32_t w, h, t; // digit box and bar thickness
} clock_size_t;

static clock_size_t const SAVER_SIZE = {110, 200, 22};
static clock_size_t const LOCK_SIZE  = {60, 110, 12};

// LVGL task
static lv_obj_t    *saver_scr;
static big_clock_t  saver_clock;
static lv_timer_t  *saver_timer;
static uint32_t     saver_step = UINT32_MAX;
static lv_obj_t    *lock_scr;
static big_clock_t  lock_clock;
static lv_obj_t    *lock_bar;
static lv_timer_t  *lock_timer;
static lock_combo_t combo;
static atomic_bool  locked; // read from any task
static int          saver_style;     // the style on screen
static int          saver_asked;     // the style it was built for (differs when the dial had no PSRAM)
static atomic_int   style_setting;   // Settings > Display (screen_saver_reload)
static atomic_int   style_test = -1; // "fw screensaver test <style>" until the saver goes; -1: none

static lv_obj_t *add_rect(lv_obj_t *parent, seg_rect_t r) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
    lv_obj_set_style_radius(o, r.w < r.h ? r.w / 2 : r.h / 2, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

static lv_obj_t *add_label(lv_obj_t *parent, int font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, lichen_font(font), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

// The digits sit in a fixed-size row; the box is a column of that row, the date and the unread count
static void clock_create(big_clock_t *c, lv_obj_t *parent, clock_size_t s) {
    memset(c, 0, sizeof(*c));
    c->box = lv_obj_create(parent);
    lv_obj_remove_style_all(c->box);
    lv_obj_set_size(c->box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c->box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c->box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c->box, s.t, 0);
    lv_obj_t *row = lv_obj_create(c->box);
    lv_obj_remove_style_all(row);
    int32_t gap = s.t;
    lv_obj_set_size(row, 4 * s.w + 4 * gap + s.t, s.h);
    for (int d = 0; d < 4; d++) {
        int32_t x = d * (s.w + gap) + (d >= 2 ? s.t + gap : 0);
        for (int g = 0; g < SEG7_COUNT; g++) {
            seg_rect_t r = seg7_rect(g, s.w, s.h, s.t);
            r.x += x;
            c->seg[d][g] = add_rect(row, r);
        }
    }
    int32_t cx = 2 * (s.w + gap);
    c->colon[0] = add_rect(row, (seg_rect_t){cx, s.h * 3 / 10 - s.t / 2, s.t, s.t});
    c->colon[1] = add_rect(row, (seg_rect_t){cx, s.h * 7 / 10 - s.t / 2, s.t, s.t});
    c->date     = add_label(c->box, LICHEN_FONT_LARGE, THEME_TEXT);
    c->notes    = add_label(c->box, LICHEN_FONT_SMALL, THEME_AMBER);
}

// HH:MM in teal; amber and marked when the time is unverified (mesh time, as the status bar's ~HH:MM); a dimmed dash
// when the clock is not set
static void clock_update(big_clock_t *c) {
    time_t    now = time(NULL);
    struct tm local;
    char      digits[8] = "----";
    char      date[48];
    uint32_t  color = THEME_DIM;
    if (saver_date_text(date, sizeof(date)) && localtime_r(&now, &local)) {
        snprintf(digits, sizeof(digits), "%02d%02d", local.tm_hour % 100, local.tm_min % 100);
        color = clock_is_approximate() ? THEME_AMBER : THEME_TEAL;
    }
    char key[sizeof(c->shown)];
    snprintf(key, sizeof(key), "%s%c", digits, color == THEME_TEAL ? 't' : color == THEME_AMBER ? 'a' : 'd');
    if (strcmp(key, c->shown) != 0) {
        strcpy(c->shown, key);
        for (int d = 0; d < 4; d++) {
            uint8_t mask = seg7_mask(digits[d]);
            for (int g = 0; g < SEG7_COUNT; g++) {
                lv_obj_set_style_bg_color(c->seg[d][g], lv_color_hex(color), 0);
                lv_obj_set_hidden(c->seg[d][g], !(mask & (1u << g)));
            }
        }
        for (int i = 0; i < 2; i++) {
            lv_obj_set_style_bg_color(c->colon[i], lv_color_hex(color), 0);
        }
    }
    if (strcmp(lv_label_get_text(c->date), date) != 0) {
        lv_label_set_text(c->date, date);
    }
    saver_notes_update(c->notes);
}

bool saver_date_text(char *out, size_t cap) {
    time_t    now = time(NULL);
    struct tm local;
    if (!clock_is_set() || !localtime_r(&now, &local)) {
        snprintf(out, cap, "Clock not set");
        return false;
    }
    char day[16], month[16];
    strftime(day, sizeof(day), "%A", &local);
    strftime(month, sizeof(month), "%B", &local);
    snprintf(out, cap, "%s%s %d %s", clock_is_approximate() ? "~ " : "", day, local.tm_mday, month);
    return true;
}

void saver_notes_update(lv_obj_t *notes) {
    size_t unread = lichen_notify_unread(NULL);
    char   text[40];
    snprintf(text, sizeof(text), "%u unread notification%s", (unsigned)unread, unread == 1 ? "" : "s");
    lv_obj_set_hidden(notes, unread == 0);
    if (unread && strcmp(lv_label_get_text(notes), text) != 0) {
        lv_label_set_text(notes, text);
    }
}

// The cover: the saver while it is up (the lock screen waits under it), else the lock screen, else none
static void cover_update(void) {
    shell_set_cover(saver_scr ? saver_scr : lock_scr, saver_scr == NULL);
}

// Screensaver

static void saver_place(void) {
    uint32_t step = lv_tick_get() / DRIFT_MS;
    if (step == saver_step) {
        return;
    }
    saver_step = step;
    lv_obj_update_layout(saver_scr);
    int32_t span_x = (lv_obj_get_width(saver_scr) - lv_obj_get_width(saver_clock.box)) / 2 - EDGE;
    int32_t span_y = (lv_obj_get_height(saver_scr) - lv_obj_get_height(saver_clock.box)) / 2 - EDGE;
    int32_t x, y;
    saver_drift(step, span_x, span_y, &x, &y);
    lv_obj_align(saver_clock.box, LV_ALIGN_CENTER, x, y);
}

static void saver_tick(lv_timer_t *t) {
    (void)t;
    clock_update(&saver_clock);
    saver_place();
}

// The style to show: a console test's, else the setting
static int saver_style_wanted(void) {
    int test = atomic_load(&style_test);
    return test >= 0 ? test : atomic_load(&style_setting);
}

static void saver_build(void) {
    saver_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(saver_scr);
    lv_obj_set_style_bg_color(saver_scr, lv_color_black(), 0); // as little light as the panel can give
    lv_obj_set_style_bg_opa(saver_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(saver_scr, LV_SCROLLBAR_MODE_OFF);
    saver_asked = saver_style_wanted();
    saver_style = saver_asked;
    if (saver_style == SAVER_STYLE_STATION && saver_station_create(saver_scr)) {
        return; // its own timer
    }
    saver_style = SAVER_STYLE_DIGITAL;
    clock_create(&saver_clock, saver_scr, SAVER_SIZE);
    lv_obj_set_style_text_color(saver_clock.date, lv_color_hex(THEME_DIM), 0);
    clock_update(&saver_clock);
    saver_step = UINT32_MAX;
    saver_place();
    saver_timer = lv_timer_create(saver_tick, SAVER_TICK_MS, NULL);
}

static void saver_close(void) {
    lv_obj_t *old = saver_scr;
    saver_scr     = NULL;
    cover_update(); // the screen below is showing before the saver goes
    if (saver_timer) {
        lv_timer_delete(saver_timer);
        saver_timer = NULL;
    }
    lv_obj_delete(old);
    if (saver_style == SAVER_STYLE_STATION) {
        saver_station_delete();
    }
}

void screen_saver_show(bool on) {
    if (on && saver_scr && saver_style_wanted() != saver_asked) {
        saver_close(); // another style asked for while it shows: rebuilt below, before anything is drawn
        ESP_LOGI(TAG, "screensaver style changed");
    }
    if (on == (saver_scr != NULL)) {
        return;
    }
    if (on) {
        saver_build();
        cover_update();
        ESP_LOGI(TAG, "screensaver on (%s)", saver_style == SAVER_STYLE_STATION ? "station clock" : "digital");
        return;
    }
    saver_close();
    atomic_store(&style_test, -1); // a test's style lasts until its key
    ESP_LOGI(TAG, "screensaver off");
}

void screen_saver_reload(void) {
    bool digital = settings_get_u8(IDLE_SAVER_STYLE_KEY, SAVER_STYLE_STATION) == SAVER_STYLE_DIGITAL;
    atomic_store(&style_setting, digital ? SAVER_STYLE_DIGITAL : SAVER_STYLE_STATION);
}

// Pocket lock

static void on_lock_key(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    lock_combo_event(&combo, ev->code, ev->pressed, lv_tick_get());
}

static void lock_tick(lv_timer_t *t) {
    (void)t;
    uint32_t now = lv_tick_get();
    if (lock_combo_tick(&combo, now)) {
        ESP_LOGI(TAG, "unlocked (combo)");
        screen_lock_release();
        return;
    }
    lv_bar_set_value(lock_bar, lock_combo_percent(&combo, now), LV_ANIM_OFF);
    static uint32_t clock_at;
    if (now - clock_at >= SAVER_TICK_MS) { // the bar wants 50 ms, the clock a second
        clock_at = now;
        clock_update(&lock_clock);
    }
}

static lv_obj_t *add_text(lv_obj_t *parent, char const *text, int font, uint32_t color) {
    lv_obj_t *l = add_label(parent, font, color);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    return l;
}

static void lock_build(void) {
    lock_scr = lv_obj_create(NULL);
    shell_style_screen(lock_scr);
    lv_obj_set_style_pad_top(lock_scr, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_hor(lock_scr, THEME_PAD, 0);
    lv_obj_set_style_pad_bottom(lock_scr, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(lock_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(lock_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lock_scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(lock_scr, 20, 0);
    clock_create(&lock_clock, lock_scr, LOCK_SIZE);
    lv_obj_set_style_text_font(lock_clock.date, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_text_color(lock_clock.date, lv_color_hex(THEME_DIM), 0);
    clock_update(&lock_clock);
    lv_obj_set_style_margin_bottom(lock_clock.box, 24, 0);
    add_text(lock_scr, "Locked", LICHEN_FONT_LARGE, THEME_TEXT);
    add_text(lock_scr, "Hold " LOCK_COMBO_TEXT " together to unlock", LICHEN_FONT_SMALL, THEME_TEXT);
    lock_bar = lv_bar_create(lock_scr);
    lv_obj_set_size(lock_bar, 360, 12);
    lv_bar_set_range(lock_bar, 0, 100);
    lv_obj_set_style_bg_color(lock_bar, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(lock_bar, lv_color_hex(THEME_TEAL), LV_PART_INDICATOR);
    add_text(lock_scr, "A pocket lock against stray key presses, not security: there is no PIN.", LICHEN_FONT_SMALL,
             THEME_DIM);
}

void screen_lock_engage(void) {
    if (lock_scr) {
        return;
    }
    lock_combo_reset(&combo);
    // Without the listener nothing could unlock it: then no lock at all
    if (!raw_keys_listen(on_lock_key, NULL, true)) {
        ESP_LOGE(TAG, "no raw key listener: not locking");
        return;
    }
    lock_build();
    lock_timer = lv_timer_create(lock_tick, LOCK_TICK_MS, NULL);
    atomic_store(&locked, true);
    cover_update();
    ESP_LOGI(TAG, "locked");
}

void screen_lock_release(void) {
    if (!lock_scr) {
        return;
    }
    raw_keys_unlisten(on_lock_key, NULL);
    if (lock_timer) {
        lv_timer_delete(lock_timer);
        lock_timer = NULL;
    }
    lv_obj_t *old = lock_scr;
    lock_scr      = NULL;
    lock_bar      = NULL;
    atomic_store(&locked, false);
    cover_update();
    lv_obj_delete(old);
}

void screen_lock_probe_key(uint32_t lv_key) {
    if (lock_scr) {
        lock_combo_probe_key(&combo, lv_key, lv_tick_get());
    }
}

void screen_lock_probe_end(void) {
    if (lock_scr) {
        lock_combo_reset(&combo); // a script that stopped half-way must not leave keys "held"
    }
}

bool screen_lock_active(void) {
    return atomic_load(&locked);
}

bool lichen_screen_locked(void) {
    return screen_lock_active();
}

void screen_lock_wake(void) {
    lvgl_port_lock(0);
    screen_lock_release();
    screen_idle_key(); // a fresh idle clock: the saver goes and the backlight comes up on the next tick
    lvgl_port_unlock();
}

// Console (bench)

static bool setting(char const *key, bool def) {
    return settings_get_u8(key, def) != 0;
}

static bool set_setting(char const *key, bool on) {
    if (!settings_set_u8(key, on)) {
        console_reply("could not save the setting");
        return false;
    }
    screen_idle_reload();
    return true;
}

#define SAVER_USAGE "screensaver [on|off|test [analog|digital]]"

static char const *style_name(int style) {
    return style == SAVER_STYLE_DIGITAL ? "digital" : "analog (station clock)";
}

// "fw screensaver test [analog|digital]": the saver now, in the setting's style or the one named, until the next key
static void saver_test(char const *style) {
    int test = -1;
    if (strcmp(style, "analog") == 0) {
        test = SAVER_STYLE_STATION;
    } else if (strcmp(style, "digital") == 0) {
        test = SAVER_STYLE_DIGITAL;
    } else if (*style) {
        console_reply(SAVER_USAGE);
        return;
    }
    atomic_store(&style_test, test);
    screen_idle_force_saver();
    console_reply("screensaver (%s) shown until the next key (needs home on screen, nothing holding it bright)",
                  style_name(test >= 0 ? test : atomic_load(&style_setting)));
}

// "fw screensaver [on|off|test [analog|digital]]"
static void cmd_saver(char const *args) {
    if (strncmp(args, "test", 4) == 0 && (args[4] == '\0' || args[4] == ' ')) {
        char const *style = args + 4;
        while (*style == ' ') {
            style++;
        }
        saver_test(style);
        return;
    }
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        if (!set_setting(IDLE_SAVER_KEY, args[1] == 'n')) {
            return;
        }
    } else if (*args) {
        console_reply(SAVER_USAGE);
        return;
    }
    console_reply("screensaver %s (the dim stage), %s, screen %s", setting(IDLE_SAVER_KEY, true) ? "on" : "off",
                  style_name(atomic_load(&style_setting)), screen_idle_stage_name());
}

// "fw lock [on|off|now|unlock]"
static void cmd_lock(char const *args) {
    if (strcmp(args, "now") == 0 || strcmp(args, "unlock") == 0) {
        lvgl_port_lock(0);
        bool home = shell_home_shown();
        if (args[0] == 'n' && home) {
            screen_lock_engage();
            screen_idle_key(); // lit, so the lock screen shows
        } else if (args[0] == 'u') {
            screen_lock_release();
        }
        lvgl_port_unlock();
        if (args[0] == 'n' && !home) {
            console_reply("not now: home is not up yet");
            return;
        }
    } else if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        bool on = args[1] == 'n';
        if (!set_setting(IDLE_LOCK_KEY, on)) {
            return;
        }
        if (!on) {
            lvgl_port_lock(0);
            screen_lock_release();
            lvgl_port_unlock();
        }
    } else if (*args) {
        console_reply("lock [on|off|now|unlock] (pocket lock: setting, lock at once, lift it)");
        return;
    }
    console_reply("lock %s, %s (unlock: hold %s for %u ms)", setting(IDLE_LOCK_KEY, false) ? "on" : "off",
                  screen_lock_active() ? "locked" : "unlocked", LOCK_COMBO_TEXT, (unsigned)LOCK_HOLD_MS);
}

void screen_saver_init(void) {
    screen_saver_reload();
    console_register("screensaver", SAVER_USAGE " (clock screensaver: setting, or show it now)", cmd_saver);
    console_register("lock", "lock [on|off|now|unlock] (pocket lock: setting, lock at once, lift it)", cmd_lock);
}
