// "Updating Lichen" (#76): the screen a Wi-Fi update shows while it runs, and the line on home after the reboot into it.
// The OTA task (core 0) calls begin/progress/end, which only store numbers under a spinlock; an LVGL timer (5 Hz) opens
// the screen and refreshes it, so the OTA task never waits on LVGL.
#include "lichen_ui.h"
#include "screen_ids.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "lichen_sys_update.h"
#include "lichen_sys_ui.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "logo.h"
#include "nav.h"
#include "notes_app.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "fonts.h"
#include "theme.h"
#include "esp_system.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "shell.h"
#include "update_mode.h"
#include "update_screen.h"
#include "update_status.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define TICK_MS         200
#define FILE_MIN_MS     1000  // a small file would otherwise flash past
#define FAILED_MS       3000
#define BANNER_MS       4000
#define UPD_FROM_KEY    "upd_from"

static char const TAG[] = "update";

typedef enum { PH_IDLE, PH_RECEIVING, PH_RESTARTING, PH_FILE_DONE, PH_FAILED } phase_t;

typedef struct {
    phase_t  phase;
    uint32_t gen; // counts begins
    bool     firmware; // an image (firmware or assets, #22): update mode, ends in a restart
    bool     new_version; // real firmware: remembered for "Updated" / "Update failed" after the restart
    bool     installed;   // new firmware installed: the restart boots it, so it never waits for the trial (#304)
    bool     hold;        // the sender holds the restart (X-Reboot: hold, #298): no self-restart after 2 s
    char     name[48];
    uint32_t total;
    uint32_t done;
    int64_t  stamp_ms; // last news from the OTA task
} state_t;

static state_t     shared; // under lock
static update_state_t um;    // under lock: update mode, and the restart decision (update_mode.c)
static bool        close_pending;  // update mode asked for the screens to go: the LVGL timer does it
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// Everything not needed to receive the image stops. Runs on the OTA task (it may block on the mesh saving its files),
// before the transfer starts; the screens go on the LVGL task (close_pending).
bool shell_stop_apps(void) {
    bool all_stopped = true;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app || !app->stop_for_update) {
            continue;
        }
        lichen_ctx_t previous = lichen_ctx_enter(lichen_sys_app_ctx_at(i)); // #230: on the OTA task, for the app
        bool         stopped  = app->stop_for_update();
        lichen_ctx_leave(previous);
        if (!stopped) {
            ESP_LOGE(TAG, "%s is still running", app->name);
            all_stopped = false;
        }
    }
    return all_stopped;
}

static void stop_everything(void) {
    ESP_LOGI(TAG, "firmware update: stopping the apps");
    update_mode_run_stop_hook(shell_stop_apps());
}

// POST /prepare (#76): everything stops now, before the first byte of the image is on its way. The update screen comes
// up ("Updating Lichen", 0 KB) and the screens close; returns when that is done (or after 5 s if the screen can't open).
void update_screen_prepare(void) {
    portENTER_CRITICAL(&lock);
    unsigned act = update_mode_on_prepare(&um, now_ms());
    if (act & UM_ACT_CLOSE_SCREENS) {
        close_pending = true;
    }
    shared.phase    = PH_RECEIVING;
    shared.gen++;
    shared.firmware    = true;
    shared.new_version = false; // the begin that follows says whether this is new firmware or assets
    shared.installed   = false;
    shared.hold        = false;
    shared.name[0]     = '\0';
    shared.total    = 0;
    shared.done     = 0;
    shared.stamp_ms = now_ms();
    portEXIT_CRITICAL(&lock);
    if (act & UM_ACT_STOP_APPS) {
        stop_everything();
    }
    for (int i = 0; i < 100; i++) {
        portENTER_CRITICAL(&lock);
        bool closing = close_pending;
        portEXIT_CRITICAL(&lock);
        if (!closing) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

bool update_screen_file_ok(void) {
    portENTER_CRITICAL(&lock);
    bool ok = update_mode_file_allowed(&um);
    portEXIT_CRITICAL(&lock);
    return ok;
}

void update_screen_begin(bool image, bool firmware, char const *name, uint32_t total) {
    portENTER_CRITICAL(&lock);
    unsigned act = update_mode_on_begin(&um, image);
    if (act & UM_ACT_REFUSE) {
        portEXIT_CRITICAL(&lock); // the server answers 409 before it gets here; nothing of ours may change
        return;
    }
    if (act & UM_ACT_CLOSE_SCREENS) {
        close_pending = true;
    }
    shared.phase    = PH_RECEIVING;
    shared.gen++;
    shared.firmware    = image;
    shared.new_version = firmware;
    shared.installed   = false;
    shared.hold        = false;
    snprintf(shared.name, sizeof(shared.name), "%s", name ? name : "");
    shared.total    = total;
    shared.done     = 0;
    shared.stamp_ms = now_ms();
    portEXIT_CRITICAL(&lock);
    if (act & UM_ACT_STOP_APPS) {
        stop_everything();
        portENTER_CRITICAL(&lock);
        shared.stamp_ms = now_ms(); // the stop may have taken seconds: the stall timer starts after it
        portEXIT_CRITICAL(&lock);
    }
}

void update_screen_progress(uint32_t done) {
    portENTER_CRITICAL(&lock);
    if (shared.phase == PH_RECEIVING) {
        shared.done     = done;
        shared.stamp_ms = now_ms();
    }
    portEXIT_CRITICAL(&lock);
}

void update_screen_end(bool ok, bool rebooting) {
    // An installed image shows "Restarting" either way; rebooting false means the sender holds the restart (#298)
    bool save = false;
    portENTER_CRITICAL(&lock);
    // FAILED too: the stall timer may have given up on a slow flash write that then succeeded
    if (shared.phase == PH_RECEIVING || (shared.phase == PH_FAILED && ok)) {
        if (ok) {
            shared.phase = shared.firmware ? PH_RESTARTING : PH_FILE_DONE;
            shared.done  = shared.total;
            save         = shared.new_version;
            shared.installed = shared.new_version;
            shared.hold      = !rebooting;
        } else {
            shared.phase = PH_FAILED;
        }
        update_mode_on_end(&um, shared.firmware, ok);
        shared.stamp_ms = now_ms();
    }
    portEXIT_CRITICAL(&lock);
    if (save) {
        // The next boot compares this with what it runs, to say "Updated" or "Update failed, back on"
        settings_set_str(UPD_FROM_KEY, esp_app_get_description()->version);
    }
}

static state_t snapshot(void) {
    portENTER_CRITICAL(&lock);
    state_t s = shared;
    portEXIT_CRITICAL(&lock);
    return s;
}

// --- the screen (LVGL task) ---

static lv_obj_t *title_label;
static lv_obj_t *bar;
static lv_obj_t *status_label;
static lv_obj_t *hint_label;
static uint32_t  opened_gen; // the transfer the screen was opened (or refreshed) for
static int64_t   opened_ms;
static bool      open_failed_warned;
static bool      restart_held; // the restart is due but waits for this version's valid mark (#304)

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color, lv_font_t const *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, text);
    return label;
}

static void set_text(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    title_label = bar = status_label = hint_label = NULL;
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    shell_style_screen(screen);
    lv_obj_set_style_pad_all(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(screen, 20, 0);
    lv_obj_set_scrollable(screen, false);

    logo_create(screen, 100);
    title_label = add_label(screen, "Updating Lichen", THEME_TEXT, lichen_font(LICHEN_FONT_LARGE));
    lv_obj_set_width(title_label, LV_PCT(100));
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    bar = lv_bar_create(screen);
    lv_obj_set_size(bar, LV_PCT(60), 12);
    lv_obj_set_style_margin_top(bar, 16, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    status_label = add_label(screen, "", THEME_AMBER, NULL);
    hint_label  = add_label(screen, "Don't turn off the badge", THEME_DIM, NULL);
    lv_obj_set_style_margin_top(hint_label, 24, 0);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    return screen;
}

static void render(state_t const *s) {
    char text[80];
    if (s->firmware) {
        set_text(title_label, "Updating Lichen");
    } else {
        snprintf(text, sizeof(text), "Receiving %s", s->name);
        set_text(title_label, text);
    }
    int         pct = update_pct(s->done, s->total);
    uint32_t    color = THEME_AMBER;
    char const *status;
    switch (s->phase) {
        case PH_RESTARTING:
            status = "Restarting...";
            break;
        case PH_FILE_DONE:
            status = "Done";
            color  = THEME_TEAL;
            break;
        case PH_FAILED:
            status = "Update failed: the new version did not install";
            color  = THEME_RED;
            break;
        default:
            if (s->firmware && s->total == 0 && s->done == 0) {
                status = "Preparing..."; // /prepare came, the image has not started
            } else if (s->firmware && s->total && s->done >= s->total) {
                status = "Checking the update...";
            } else if (pct >= 0) {
                snprintf(text, sizeof(text), "%d%%", pct);
                status = text;
            } else {
                snprintf(text, sizeof(text), "%lu KB", (unsigned long)(s->done / 1024)); // total unknown
                status = text;
            }
    }
    set_text(status_label, status);
    lv_obj_set_style_text_color(status_label, lv_color_hex(color), 0);
    lv_bar_set_value(bar, s->phase == PH_FAILED ? 0 : (s->phase == PH_IDLE || pct < 0 ? 0 : pct), LV_ANIM_OFF);
    bool busy = s->phase == PH_RECEIVING || s->phase == PH_RESTARTING;
    bool failed = s->phase == PH_FAILED;
    char const *hint = restart_held ? "Waiting for this version to be confirmed, then the badge restarts"
                       : failed     ? "The badge still runs the old version. Start the update again from your computer."
                                    : "Don't turn off the badge";
    set_text(hint_label, hint);
    lv_obj_set_hidden(hint_label, !(busy || failed || restart_held));
}

static void finish_screen(uint32_t gen) {
    if (restart_held) {
        return; // the screen stays up with its waiting line until the restart goes (#304)
    }
    // Back to what was open before; only when this screen is still the one showing
    if (title_label && nav_current() == SCREEN_UPDATE) {
        shell_back();
    }
    portENTER_CRITICAL(&lock);
    if (shared.gen == gen && shared.phase != PH_RECEIVING) {
        shared.phase = PH_IDLE;
    }
    portEXIT_CRITICAL(&lock);
}

// On the LVGL task: the gate's wait (system_restart on trial, #304) would freeze the UI for up to two minutes, so a
// restart that would wait is held and tried again on the next tick. A good install restarts at once: it boots the new
// version, the trial of this one no longer matters.
static void restart_after_update(bool due, state_t const *s) {
    if (!due) {
        restart_held = false; // not due (any more): a new prepare or begin took the restart back
        return;
    }
    if (s->installed) {
        ESP_LOGI(TAG, "restarting after the update");
        system_restart_asked("after update");
    }
    if (system_restart_would_wait()) {
        if (!restart_held) {
            ESP_LOGW(TAG, "restart after the update held: this version is still on trial");
            restart_held = true;
        }
        return;
    }
    ESP_LOGI(TAG, "restarting after the update");
    system_restart("after update");
}

// /prepare that no firmware followed is a failed update; until then the stall rule has nothing to say. Returns whether
// the badge still waits for the image after a prepare.
static bool tick_prepare_expiry(int64_t now) {
    portENTER_CRITICAL(&lock);
    bool expired = (update_mode_tick(&um, now) & UM_ACT_REBOOT) != 0;
    bool waiting = um.prepared;
    if (expired) {
        shared.phase    = PH_FAILED;
        shared.stamp_ms = now;
    }
    portEXIT_CRITICAL(&lock);
    if (expired) {
        ESP_LOGW(TAG, "prepared, but no firmware came: restarting");
    }
    return waiting;
}

// A sender that went quiet must not keep the screen forever, and an installed image nobody rebooted restarts by itself
// (update_stall, #298). s follows what was decided.
static void tick_stall(state_t *s, bool waiting, int64_t now) {
    upd_watch_t const w     = {.waiting    = waiting,
                               .receiving  = s->phase == PH_RECEIVING,
                               .restarting = s->phase == PH_RESTARTING,
                               .hold       = s->hold,
                               .firmware   = s->firmware,
                               .done       = s->done,
                               .total      = s->total,
                               .quiet_ms   = now - s->stamp_ms};
    upd_stall_t const stall = update_stall(&w);
    if (stall == UPD_STALL_NONE) {
        return;
    }
    portENTER_CRITICAL(&lock);
    if (shared.gen == s->gen && shared.stamp_ms == s->stamp_ms) {
        if (stall == UPD_STALL_SENDER) {
            shared.phase = PH_FAILED;
            update_mode_on_end(&um, s->firmware, false); // everything else is stopped: restart after the message
        } else {
            shared.phase = PH_IDLE;
            update_mode_on_restart_stall(&um); // installed, nobody rebooted it: restart now
        }
        shared.stamp_ms = now;
        s->phase        = shared.phase;
        s->stamp_ms     = now;
    }
    portEXIT_CRITICAL(&lock);
    if (s->phase == PH_FAILED) {
        ESP_LOGW(TAG, "the sender stopped; giving the screen back");
    }
}

// The restart update mode decided, once its message has been shown. Returns whether a restart is decided at all.
static bool tick_restart(state_t const *s, int64_t now) {
    bool shown = (s->phase == PH_FAILED && now - s->stamp_ms >= FAILED_MS) || s->phase == PH_IDLE ||
                 (s->phase == PH_RESTARTING && now - s->stamp_ms >= FAILED_MS); // installed, and the restart was decided
    portENTER_CRITICAL(&lock);
    bool restart         = update_mode_restart_due(&um, shown);
    bool restart_decided = um.reboot;
    portEXIT_CRITICAL(&lock);
    restart_after_update(restart, s);
    return restart_decided;
}

// Open the screen for a transfer that has none yet; false when there is nothing to draw on this tick
static bool tick_open(state_t const *s, int64_t now) {
    if (title_label) {
        if (s->gen != opened_gen) {
            opened_gen = s->gen; // the next file of a series: same screen
            opened_ms  = now;
        }
        return true;
    }
    if (s->phase == PH_IDLE) {
        return false;
    }
    if (s->gen == opened_gen) { // closed with Esc: leave it closed
        if (s->phase == PH_FILE_DONE || s->phase == PH_FAILED) {
            finish_screen(s->gen);
        }
        return false;
    }
    if (!shell_home_shown()) {
        return false; // still booting
    }
    shell_open(build, SCREEN_UPDATE); // takes the lock itself; we are on the LVGL task already
    if (!title_label) {
        if (!open_failed_warned) {
            ESP_LOGW(TAG, "couldn't open the update screen");
            open_failed_warned = true;
        }
        return false;
    }
    open_failed_warned = false;
    opened_ms          = now;
    opened_gen         = s->gen;
    return true;
}

static void tick_close_screens(void) {
    portENTER_CRITICAL(&lock);
    bool close = close_pending && nav_current() == SCREEN_UPDATE;
    if (close) {
        close_pending = false;
    }
    portEXIT_CRITICAL(&lock);
    if (close) {
        shell_close_all_but_top(); // frees home and whatever was open: the update needs the internal RAM
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    int64_t now     = now_ms();
    bool    waiting = tick_prepare_expiry(now);
    state_t s       = snapshot();
    tick_stall(&s, waiting, now);
    bool restart_decided = tick_restart(&s, now);
    if (!tick_open(&s, now)) {
        return;
    }
    tick_close_screens();
    render(&s);
    bool file_shown   = s.phase == PH_FILE_DONE && now - opened_ms >= FILE_MIN_MS && now - s.stamp_ms >= 0;
    bool failed_shown = s.phase == PH_FAILED && now - s.stamp_ms >= FAILED_MS;
    if (file_shown || failed_shown || (s.phase == PH_IDLE && !restart_decided)) {
        finish_screen(s.gen); // PH_IDLE without a restart: the stall timer gave up on a restart that never came
    }
}

void update_screen_init(void) {
    lv_timer_create(tick, TICK_MS, NULL);
}

// --- after the reboot ---

static char     banner_line[160]; // "Update failed, back on <git-describe>" wraps on the banner, so it is not cut here (#132)
static atomic_int boot_result = UPD_NONE; // set once banner_line holds the text (#62 reads it from the LVGL task)
static lv_obj_t *banner;
static int64_t  banner_shown_ms;
static atomic_bool banner_up; // the banner is waiting to show or showing (the release notes screen waits for it, #13)

static void banner_tick(lv_timer_t *t) {
    if (!banner) {
        if (!shell_home_shown()) {
            return;
        }
        banner = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(banner);
        lv_obj_set_size(banner, LV_PCT(90), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(banner, lv_color_hex(THEME_TRACK), 0);
        lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(banner, 2, 0);
        lv_obj_set_style_border_color(banner, lv_color_hex(strncmp(banner_line, "Update failed", 13) == 0 ? THEME_RED : THEME_AMBER), 0);
        lv_obj_set_style_radius(banner, 12, 0);
        lv_obj_set_style_pad_hor(banner, 28, 0);
        lv_obj_set_style_pad_ver(banner, 14, 0);
        lv_obj_set_style_text_color(banner, lv_color_hex(THEME_TEXT), 0);
        lv_obj_set_style_text_font(banner, lichen_font(LICHEN_FONT_SMALL), 0);
        lv_obj_set_scrollable(banner, false);
        lv_obj_set_clickable(banner, false);
        lv_obj_t *label = lichen_label_wrap(banner, LV_PCT(100));
        lv_label_set_text(label, banner_line);
        lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, THEME_STATUS_BAR_H + 12);
        banner_shown_ms = now_ms();
        return;
    }
    if (now_ms() - banner_shown_ms >= BANNER_MS) {
        lv_obj_delete(banner);
        banner = NULL;
        atomic_store(&banner_up, false);
        lv_timer_delete(t);
    }
}

static bool erase_pending; // upd_from stays until the running image is valid

void update_screen_image_valid(void) {
    if (erase_pending) {
        settings_erase(UPD_FROM_KEY);
        erase_pending = false;
    }
}

bool update_screen_blocks_back(void) {
    // A firmware update keeps "Don't turn off the badge" on screen; a file push can be closed
    portENTER_CRITICAL(&lock);
    bool busy = shared.firmware && (shared.phase == PH_RECEIVING || shared.phase == PH_RESTARTING);
    portEXIT_CRITICAL(&lock);
    return busy && nav_current() == SCREEN_UPDATE;
}

bool update_screen_busy(void) {
    portENTER_CRITICAL(&lock);
    bool busy = um.mode == UM_UPDATING || shared.phase == PH_RECEIVING || shared.phase == PH_RESTARTING;
    portEXIT_CRITICAL(&lock);
    return busy;
}

bool update_screen_banner_active(void) {
    return atomic_load(&banner_up);
}

// Says what the update did; returns the result for the release notes (#13)
static upd_result_t boot_check(void) {
    char prev[64];
    settings_get_str(UPD_FROM_KEY, prev, sizeof(prev), "");
    if (!prev[0]) {
        return UPD_NONE;
    }
    // The bootloader marks an image that failed its trial; if that is not the one running, it went back
    bool        rolled = lichen_sys_update_rolled_back();
    bool        valid  = !lichen_sys_update_on_trial();
    char const *run    = esp_app_get_description()->version;
    upd_boot_t  after = update_after_boot(prev, run, rolled, valid);
    ESP_LOG_LEVEL(
        rolled ? ESP_LOG_WARN : ESP_LOG_INFO,
        TAG,
        "update from %s, now %s%s%s",
        prev,
        run,
        rolled ? " (rolled back)" : "",
        valid ? "" : " (on trial)"
    );
    if (after.erase) {
        settings_erase(UPD_FROM_KEY); // the banner text is already kept in banner_line
    } else {
        erase_pending = true; // on trial: main.c calls update_screen_image_valid() once it counts
    }
    if (after.result == UPD_NONE) {
        return UPD_NONE;
    }
    update_line(after.result, prev, run, banner_line, sizeof(banner_line));
    atomic_store(&boot_result, (int)after.result);
    atomic_store(&banner_up, true);
    lichen_sys_ui_lock(0);
    lv_timer_create(banner_tick, TICK_MS, NULL);
    lichen_sys_ui_unlock();
    return after.result;
}

void update_screen_boot_check(void) {
    notes_boot_check(boot_check() == UPD_UPDATED);
}

upd_result_t update_screen_boot_result(char *line, size_t cap) {
    upd_result_t r = (upd_result_t)atomic_load(&boot_result);
    if (cap) {
        snprintf(line, cap, "%s", r == UPD_NONE ? "" : banner_line);
    }
    return r;
}
