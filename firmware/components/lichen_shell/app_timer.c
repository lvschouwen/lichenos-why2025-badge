// Timer (#152): a countdown, a stopwatch and a world clock in one app. Home shows the app; its screen lists the three
// pages with what each is doing. Both timers live in timer_service.c, so they go on when the screen is left, and the
// tile shows the one that runs. Nothing here writes flash. At zero the service raises a notice (toast, status dot, tile
// badge); this file is only the screens. Only the OS API: menu, text entry, nav, theme; time from the C library.
#include "app_icons.h"
#include "fonts.h"
#include "lichen_app.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "menu.h"
#include "screen_ids.h"
#include "shell_parts.h"
#include "text_entry.h"
#include "theme.h"
#include "timer_calc.h"
#include "timer_service.h"
#include "tz_table.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define REFRESH_MS 100 // the stopwatch shows tenths

// ---- shared bits ----

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

// The big time on a page: LARGE, centred, text colour
static lv_obj_t *add_readout(lv_obj_t *menu) {
    lv_obj_t *label = menu_note(menu, "");
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

static void on_page_delete(lv_event_t *e) {
    lv_timer_delete(lv_event_get_user_data(e)); // before the labels go with the screen
}

// ---- Timer page ----

static struct {
    lv_obj_t *readout, *state, *toggle, *reset, *set, *quick;
    char      shown_toggle[8];
    char      shown_set[48];
    char      shown_quick[48];
    char      message[80]; // what the last key could not do, until the next one
} s_t;

static int64_t const quick_minutes[] = {1, 3, 5, 10, 15, 30, 60};

static void timer_refresh(lv_timer_t *timer) {
    (void)timer;
    tcd_t  *cd  = timer_svc_countdown();
    int64_t now = timer_svc_now();
    char    text[48];
    tfmt_countdown(text, sizeof(text), tcd_left(cd, now));
    set_text(s_t.readout, text);
    char const *state = s_t.message[0] ? s_t.message : cd->state == TCD_RUNNING ? "Running" :
                        cd->state == TCD_PAUSED ? "Paused" : cd->state == TCD_DONE ? "Time is up" : "Ready";
    set_text(s_t.state, state);
    uint32_t color = s_t.message[0] ? THEME_RED : cd->state == TCD_DONE ? THEME_AMBER :
                     cd->state == TCD_RUNNING ? THEME_TEAL : THEME_DIM;
    lv_obj_set_style_text_color(s_t.state, lv_color_hex(color), 0);
    char const *toggle = cd->state == TCD_RUNNING ? "Pause" : cd->state == TCD_PAUSED ? "Resume" : "Start";
    if (strcmp(s_t.shown_toggle, toggle) != 0) {
        snprintf(s_t.shown_toggle, sizeof(s_t.shown_toggle), "%s", toggle);
        menu_set_label(s_t.toggle, toggle);
    }
    tfmt_countdown(text, sizeof(text), cd->set_ms);
    if (strcmp(s_t.shown_set, text) != 0) {
        snprintf(s_t.shown_set, sizeof(s_t.shown_set), "%s", text);
        menu_set_value(s_t.set, text);
    }
    tfmt_words(text, sizeof(text), cd->set_ms);
    if (strcmp(s_t.shown_quick, text) != 0) {
        snprintf(s_t.shown_quick, sizeof(s_t.shown_quick), "%s", text);
        menu_set_value(s_t.quick, text);
    }
}

static void on_toggle(void *arg) {
    (void)arg;
    tcd_t  *cd  = timer_svc_countdown();
    int64_t now = timer_svc_now();
    s_t.message[0] = '\0';
    if (cd->state == TCD_RUNNING) {
        tcd_pause(cd, now);
    } else if (!tcd_start(cd, now)) {
        snprintf(s_t.message, sizeof(s_t.message), "Set a time first");
    } else {
        timer_svc_ack(); // starting again answers a finished one
    }
    timer_refresh(NULL);
}

static void on_reset(void *arg) {
    (void)arg;
    s_t.message[0] = '\0';
    tcd_reset(timer_svc_countdown());
    timer_svc_ack();
    timer_refresh(NULL);
}

// The length can change while the timer is not counting; a paused one starts over with the new length
static bool set_length(int64_t ms) {
    tcd_t *cd = timer_svc_countdown();
    s_t.message[0] = '\0';
    if (cd->state == TCD_RUNNING) {
        snprintf(s_t.message, sizeof(s_t.message), "Pause the timer first");
        return false;
    }
    tcd_reset(cd);
    tcd_set(cd, ms);
    timer_svc_ack();
    return true;
}

static void on_length(char const *text, void *arg) {
    (void)arg;
    int64_t ms;
    if (!tdur_parse(text, &ms)) {
        snprintf(s_t.message, sizeof(s_t.message), "Not a time: type minutes (5) or m:ss (5:30)");
    } else {
        set_length(ms);
    }
    timer_refresh(NULL);
}

static void on_set(void *arg) {
    (void)arg;
    tcd_t *cd = timer_svc_countdown();
    if (cd->state == TCD_RUNNING) {
        snprintf(s_t.message, sizeof(s_t.message), "Pause the timer first");
        timer_refresh(NULL);
        return;
    }
    char initial[16];
    tfmt_countdown(initial, sizeof(initial), cd->set_ms);
    text_entry_open("Length: minutes or m:ss", initial, false, 8, on_length, NULL);
}

static void on_quick(void *arg) {
    (void)arg;
    int64_t set_ms = timer_svc_countdown()->set_ms;
    size_t  n      = sizeof(quick_minutes) / sizeof(quick_minutes[0]);
    size_t  next   = 0;
    for (size_t i = 0; i < n; i++) {
        if (quick_minutes[i] * 60000 <= set_ms) {
            next = (i + 1) % n; // the first preset above the current length
        }
    }
    set_length(quick_minutes[next] * 60000);
    timer_refresh(NULL);
}

static lv_obj_t *build_timer(void) {
    memset(&s_t, 0, sizeof(s_t));
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Timer");
    s_t.readout      = add_readout(menu);
    s_t.state        = menu_note(menu, "");
    lv_obj_set_style_text_align(s_t.state, LV_TEXT_ALIGN_CENTER, 0);
    s_t.toggle = menu_add(menu, "Start", NULL, on_toggle, NULL);
    s_t.reset  = menu_add(menu, "Reset", NULL, on_reset, NULL);
    s_t.set    = menu_add(menu, "Set time", "", on_set, NULL);
    menu_set_kind(s_t.set, MENU_OPEN);
    s_t.quick = menu_add(menu, "Quick set", "", on_quick, NULL);
    menu_set_kind(s_t.quick, MENU_CYCLE);
    timer_svc_ack(); // opening the page is seeing the notice
    lv_timer_t *timer = lv_timer_create(timer_refresh, REFRESH_MS, NULL);
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, timer);
    timer_refresh(NULL);
    return screen;
}

static void open_timer(void *arg) {
    (void)arg;
    shell_open(build_timer, SCREEN_TIMER_PAGE);
}

// ---- Stopwatch page ----

#define LAPS_SHOWN 6

static struct {
    lv_obj_t *readout, *toggle, *lap, *reset, *laps;
    char      shown_toggle[8];
    size_t    shown_laps;
} s_s;

static void laps_text(char *out, size_t n) {
    sw_t   *sw = timer_svc_stopwatch();
    size_t  w  = 0;
    out[0]     = '\0';
    if (sw->laps == 0) {
        snprintf(out, n, "Laps show here: press Lap while it runs.");
        return;
    }
    for (size_t i = 0; i < LAPS_SHOWN && i < sw->laps && w < n; i++) {
        int64_t split, total;
        size_t  number = sw->laps - i;
        char    a[24], b[24];
        sw_lap_get(sw, number, &split, &total);
        tfmt_stopwatch(a, sizeof(a), split);
        tfmt_stopwatch(b, sizeof(b), total);
        w += (size_t)snprintf(out + w, n - w, "%sLap %zu   %s   total %s", i ? "\n" : "", number, a, b);
    }
    if (sw->laps > LAPS_SHOWN && w < n) {
        snprintf(out + w, n - w, "\n%zu earlier", sw->laps - LAPS_SHOWN);
    }
}

static void stopwatch_refresh(lv_timer_t *timer) {
    (void)timer;
    sw_t *sw = timer_svc_stopwatch();
    char  text[400];
    tfmt_stopwatch(text, sizeof(text), sw_elapsed(sw, timer_svc_now()));
    set_text(s_s.readout, text);
    char const *toggle = sw->running ? "Stop" : sw->acc_ms > 0 ? "Resume" : "Start";
    if (strcmp(s_s.shown_toggle, toggle) != 0) {
        snprintf(s_s.shown_toggle, sizeof(s_s.shown_toggle), "%s", toggle);
        menu_set_label(s_s.toggle, toggle);
    }
    laps_text(text, sizeof(text));
    set_text(s_s.laps, text);
}

static void on_sw_toggle(void *arg) {
    (void)arg;
    sw_t *sw = timer_svc_stopwatch();
    if (sw->running) {
        sw_stop(sw, timer_svc_now());
    } else {
        sw_start(sw, timer_svc_now());
    }
    stopwatch_refresh(NULL);
}

static void on_sw_lap(void *arg) {
    (void)arg;
    sw_lap(timer_svc_stopwatch(), timer_svc_now()); // not running or 99 laps: nothing to mark
    stopwatch_refresh(NULL);
}

static void on_sw_reset(void *arg) {
    (void)arg;
    sw_reset(timer_svc_stopwatch());
    stopwatch_refresh(NULL);
}

static lv_obj_t *build_stopwatch(void) {
    memset(&s_s, 0, sizeof(s_s));
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Stopwatch");
    s_s.readout      = add_readout(menu);
    s_s.toggle       = menu_add(menu, "Start", NULL, on_sw_toggle, NULL);
    s_s.lap          = menu_add(menu, "Lap", NULL, on_sw_lap, NULL);
    s_s.reset        = menu_add(menu, "Reset", NULL, on_sw_reset, NULL);
    s_s.laps         = menu_note(menu, "");
    shell_page_hint(screen, "Enter", "choose", NULL);
    lv_timer_t *timer = lv_timer_create(stopwatch_refresh, REFRESH_MS, NULL);
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, timer);
    stopwatch_refresh(NULL);
    return screen;
}

static void open_stopwatch(void *arg) {
    (void)arg;
    shell_open(build_stopwatch, SCREEN_STOPWATCH_PAGE);
}

// ---- World clock page ----

static struct {
    lv_obj_t *row[TZ_MAX];
    char      shown[TZ_MAX][16];
} s_w;

static void no_action(void *arg) {
    (void)arg; // the rows only show; they are rows so the arrow keys scroll the list
}

// "Amsterdam" from "Amsterdam, Berlin, Paris": the first city names the zone
static void zone_label(char *out, size_t n, char const *name) {
    size_t len = strcspn(name, ",");
    snprintf(out, n, "%.*s", (int)len, name);
}

static void world_refresh(lv_timer_t *timer) {
    (void)timer;
    int64_t utc = (int64_t)time(NULL);
    for (size_t i = 0; i < tz_count() && i < TZ_MAX; i++) {
        wc_local_t l;
        char       text[16] = "?";
        if (wc_local(tz_at(i)->posix, utc, &l)) {
            wc_fmt(text, sizeof(text), &l);
        }
        if (strcmp(s_w.shown[i], text) != 0) {
            snprintf(s_w.shown[i], sizeof(s_w.shown[i]), "%s", text);
            menu_set_value(s_w.row[i], text);
        }
    }
}

static lv_obj_t *build_world(void) {
    memset(&s_w, 0, sizeof(s_w));
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "World clock");
    if (!clock_is_set()) {
        lv_obj_set_style_text_color(menu_note(menu, "The clock is not set, so these times are wrong. Set it in "
                                                    "Settings > Time."), lv_color_hex(THEME_AMBER), 0);
    }
    for (size_t i = 0; i < tz_count() && i < TZ_MAX; i++) {
        char label[32];
        zone_label(label, sizeof(label), tz_at(i)->name);
        s_w.row[i] = menu_add(menu, label, "", no_action, NULL);
    }
    lv_timer_t *timer = lv_timer_create(world_refresh, 1000, NULL);
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, timer);
    world_refresh(NULL);
    return screen;
}

static void open_world(void *arg) {
    (void)arg;
    shell_open(build_world, SCREEN_WORLD_PAGE);
}

// ---- the app's own screen: the three pages ----

static struct {
    lv_obj_t *timer, *stopwatch;
    char      shown_timer[24], shown_stopwatch[24];
} s_h;

static void hub_refresh(lv_timer_t *timer) {
    (void)timer;
    tcd_t  *cd  = timer_svc_countdown();
    sw_t   *sw  = timer_svc_stopwatch();
    int64_t now = timer_svc_now();
    char    text[24];
    if (cd->state == TCD_DONE) {
        snprintf(text, sizeof(text), "Time is up");
    } else {
        tfmt_countdown(text, sizeof(text), tcd_left(cd, now));
        if (cd->state == TCD_PAUSED) {
            strncat(text, " paused", sizeof(text) - strlen(text) - 1);
        }
        if (cd->state == TCD_IDLE) {
            text[0] = '\0'; // nothing running: no value
        }
    }
    if (strcmp(s_h.shown_timer, text) != 0) {
        snprintf(s_h.shown_timer, sizeof(s_h.shown_timer), "%s", text);
        menu_set_value(s_h.timer, text);
    }
    text[0] = '\0';
    if (sw->running || sw->acc_ms > 0) {
        tfmt_stopwatch(text, sizeof(text), sw_elapsed(sw, now));
        text[strlen(text) - 2] = '\0'; // whole seconds; the tenths would flicker on a list row
        if (!sw->running) {
            strncat(text, " stopped", sizeof(text) - strlen(text) - 1);
        }
    }
    if (strcmp(s_h.shown_stopwatch, text) != 0) {
        snprintf(s_h.shown_stopwatch, sizeof(s_h.shown_stopwatch), "%s", text);
        menu_set_value(s_h.stopwatch, text);
    }
}

static void open_timer_app(lv_obj_t *screen) {
    memset(&s_h, 0, sizeof(s_h));
    lv_obj_t *menu = menu_create(screen, "Timer");
    s_h.timer      = menu_add(menu, "Timer", "", open_timer, NULL);
    s_h.stopwatch  = menu_add(menu, "Stopwatch", "", open_stopwatch, NULL);
    lv_obj_t *world = menu_add(menu, "World clock", NULL, open_world, NULL);
    menu_set_kind(s_h.timer, MENU_OPEN);
    menu_set_kind(s_h.stopwatch, MENU_OPEN);
    menu_set_kind(world, MENU_OPEN);
    timer_svc_ack(); // the user is here now
    lv_timer_t *timer = lv_timer_create(hub_refresh, 500, NULL);
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, timer);
    hub_refresh(NULL);
}

// The tile: the countdown if it runs or is up, else the stopwatch if it runs
static bool timer_tile(char *l1, size_t c1, char *l2, size_t c2) {
    tcd_t  *cd  = timer_svc_countdown();
    sw_t   *sw  = timer_svc_stopwatch();
    int64_t now = timer_svc_now();
    if (cd->state == TCD_RUNNING || cd->state == TCD_PAUSED) {
        tfmt_countdown(l1, c1, tcd_left(cd, now));
        snprintf(l2, c2, "%s", cd->state == TCD_RUNNING ? "timer" : "timer paused");
        return true;
    }
    if (cd->state == TCD_DONE) {
        snprintf(l1, c1, "0:00");
        snprintf(l2, c2, "time is up");
        return true;
    }
    if (sw->running) {
        tfmt_countdown(l1, c1, sw_elapsed(sw, now) - 999); // rounds up: show whole seconds down
        snprintf(l2, c2, "stopwatch");
        return true;
    }
    return false;
}

static lichen_app_t const timer_app = {
    .name      = "Timer",
    .id        = "timer",
    .draw_icon = icon_timer,
    .tile_text = timer_tile,
    .open      = open_timer_app,
};

void timer_app_register(void) {
    timer_svc_start();
    lichen_register_app(&timer_app);
}
