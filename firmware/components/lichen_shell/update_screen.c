// "Updating Lichen" (#76): the screen a Wi-Fi update shows while it runs, and the line on home after the reboot into it.
// The OTA task (core 0) calls begin/progress/end, which only store numbers under a spinlock; an LVGL timer (5 Hz) opens
// the screen and refreshes it, so the OTA task never waits on LVGL.
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "logo.h"
#include "nav.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "fonts.h"
#include "theme.h"
#include "esp_system.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "shell.h"
#include "update_mode.h"
#include "update_screen.h"
#include "update_status.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_UPDATE   150
#define TICK_MS         200
#define FILE_MIN_MS     1000  // a small file would otherwise flash past
#define FAILED_MS       3000
#define RESTART_MS      20000 // installed but nobody asked for the reboot
#define BANNER_MS       4000
#define UPD_FROM_KEY    "upd_from"

static char const TAG[] = "update";

typedef enum { PH_IDLE, PH_RECEIVING, PH_RESTARTING, PH_FILE_DONE, PH_FAILED } phase_t;

typedef struct {
    phase_t  phase;
    uint32_t gen; // counts begins
    bool     firmware; // an image (firmware or assets, #22): update mode, ends in a restart
    bool     new_version; // real firmware: remembered for "Updated" / "Update failed" after the restart
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
static void stop_everything(void) {
    ESP_LOGW(TAG, "firmware update: stopping the apps");
    bool all_stopped = true;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app && app->stop_for_update && !app->stop_for_update()) {
            ESP_LOGE(TAG, "%s is still running", app->name);
            all_stopped = false;
        }
    }
    update_mode_run_stop_hook(all_stopped);
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
    (void)rebooting; // an installed image shows "Restarting" either way: the sender reboots it next
    bool save = false;
    portENTER_CRITICAL(&lock);
    // FAILED too: the stall timer may have given up on a slow flash write that then succeeded
    if (shared.phase == PH_RECEIVING || (shared.phase == PH_FAILED && ok)) {
        if (ok) {
            shared.phase = shared.firmware ? PH_RESTARTING : PH_FILE_DONE;
            shared.done  = shared.total;
            save         = shared.new_version;
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
            status = "Update failed";
            color  = THEME_RED;
            break;
        default:
            if (s->firmware && s->total == 0 && s->done == 0) {
                status = "Preparing..."; // /prepare came, the image has not started
            } else if (s->firmware && s->total && s->done >= s->total) {
                status = "Checking the update...";
            } else if (pct >= 0) {
                snprintf(text, sizeof(text), "%d %%", pct);
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
    lv_obj_set_hidden(hint_label, !busy);
}

static void finish_screen(uint32_t gen) {
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

static void tick(lv_timer_t *t) {
    (void)t;
    int64_t now = now_ms();

    // /prepare that no firmware followed is a failed update; until then the stall timer below has nothing to say
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
    state_t s = snapshot();

    // A sender that went quiet must not keep the screen forever
    if (!waiting && ((s.phase == PH_RECEIVING &&
                      now - s.stamp_ms > (int64_t)update_stall_limit_ms(s.firmware, s.done, s.total)) ||
                     (s.phase == PH_RESTARTING && now - s.stamp_ms > RESTART_MS))) {
        portENTER_CRITICAL(&lock);
        if (shared.gen == s.gen && shared.stamp_ms == s.stamp_ms) {
            if (s.phase == PH_RECEIVING) {
                shared.phase = PH_FAILED;
                update_mode_on_end(&um, s.firmware, false); // everything else is stopped: restart after the message
            } else {
                shared.phase = PH_IDLE;
                update_mode_on_restart_stall(&um); // installed, nobody rebooted it: restart anyway
            }
            shared.stamp_ms = now;
            s.phase         = shared.phase;
            s.stamp_ms      = now;
        }
        portEXIT_CRITICAL(&lock);
        if (s.phase == PH_FAILED) {
            ESP_LOGW(TAG, "the sender stopped; giving the screen back");
        }
    }

    portENTER_CRITICAL(&lock);
    bool restart = update_mode_restart_due(
        &um, (s.phase == PH_FAILED && now - s.stamp_ms >= FAILED_MS) || s.phase == PH_IDLE ||
                 (s.phase == PH_RESTARTING && now - s.stamp_ms >= FAILED_MS)); // installed, and the restart was decided
    bool restart_decided = um.reboot;
    portEXIT_CRITICAL(&lock);
    if (restart) {
        ESP_LOGW(TAG, "restarting after the update");
        system_restart("after update");
    }

    if (!title_label) {
        if (s.phase == PH_IDLE) {
            return;
        }
        if (s.gen == opened_gen) { // closed with Esc: leave it closed
            if (s.phase == PH_FILE_DONE || s.phase == PH_FAILED) {
                finish_screen(s.gen);
            }
            return;
        }
        if (!shell_home_shown()) {
            return; // still booting
        }
        shell_open(build, SCREEN_UPDATE); // takes the lock itself; we are on the LVGL task already
        if (!title_label) {
            if (!open_failed_warned) {
                ESP_LOGW(TAG, "couldn't open the update screen");
                open_failed_warned = true;
            }
            return;
        }
        open_failed_warned = false;
        opened_ms          = now;
        opened_gen         = s.gen;
    } else if (s.gen != opened_gen) {
        opened_gen = s.gen; // the next file of a series: same screen
        opened_ms  = now;
    }

    portENTER_CRITICAL(&lock);
    bool close = close_pending && nav_current() == SCREEN_UPDATE;
    if (close) {
        close_pending = false;
    }
    portEXIT_CRITICAL(&lock);
    if (close) {
        shell_close_all_but_top(); // frees home and whatever was open: the update needs the internal RAM
    }

    render(&s);
    if ((s.phase == PH_FILE_DONE && now - opened_ms >= FILE_MIN_MS && now - s.stamp_ms >= 0) ||
        (s.phase == PH_FAILED && now - s.stamp_ms >= FAILED_MS)) {
        finish_screen(s.gen);
    } else if (s.phase == PH_IDLE) {
        if (!restart_decided) {
            finish_screen(s.gen); // the stall timer gave up on a restart that never came
        }
    }
}

void update_screen_init(void) {
    lv_timer_create(tick, TICK_MS, NULL);
}

// --- after the reboot ---

static char     banner_line[64];
static lv_obj_t *banner;
static int64_t  banner_shown_ms;

static void banner_tick(lv_timer_t *t) {
    if (!banner) {
        if (!shell_home_shown()) {
            return;
        }
        banner = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(banner);
        lv_obj_set_size(banner, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
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
        lv_obj_t *label = lv_label_create(banner);
        lv_label_set_text(label, banner_line);
        lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, THEME_STATUS_BAR_H + 12);
        banner_shown_ms = now_ms();
        return;
    }
    if (now_ms() - banner_shown_ms >= BANNER_MS) {
        lv_obj_delete(banner);
        banner = NULL;
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

void update_screen_boot_check(void) {
    char prev[64];
    settings_get_str(UPD_FROM_KEY, prev, sizeof(prev), "");
    if (!prev[0]) {
        return;
    }
    // The bootloader marks an image that failed its trial; if that is not the one running, it went back
    esp_partition_t const *invalid = esp_ota_get_last_invalid_partition();
    esp_partition_t const *running = esp_ota_get_running_partition();
    bool                   rolled  = invalid && invalid != running;
    esp_ota_img_states_t   state;
    bool valid = esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY;
    char const *run   = esp_app_get_description()->version;
    upd_boot_t  after = update_after_boot(prev, run, rolled, valid);
    ESP_LOGW(TAG, "update from %s, now %s%s%s", prev, run, rolled ? " (rolled back)" : "", valid ? "" : " (on trial)");
    if (after.erase) {
        settings_erase(UPD_FROM_KEY); // the banner text is already kept in banner_line
    } else {
        erase_pending = true; // on trial: main.c calls update_screen_image_valid() once it counts
    }
    if (after.result == UPD_NONE) {
        return;
    }
    update_line(after.result, prev, run, banner_line, sizeof(banner_line));
    lvgl_port_lock(0);
    lv_timer_create(banner_tick, TICK_MS, NULL);
    lvgl_port_unlock();
}
