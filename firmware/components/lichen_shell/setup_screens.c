// First-run setup (#66): a welcome after the first boot, then the Wi-Fi screen with a Skip row when the badge has no
// network yet. Lives on top of home on the shell's stack, so Esc from Wi-Fi lands on the welcome. setup_done is saved
// only when the setup ends (joined, skipped, or Enter on the welcome of a badge that already has Wi-Fi).
#include "esp_log.h"
#include "lichen_app.h"
#include "esp_lvgl_port.h"
#include "fw_console.h"
#include "logo.h"
#include "settings_store.h"
#include "setup_state.h"
#include "shell_parts.h"
#include "fonts.h"
#include "theme.h"

#include <string.h>

#define SCREEN_WELCOME 101

static char const TAG[] = "setup";

static void finish(void) {
    settings_set_u8("setup_done", 1);
    shell_home();
}

// Called by the Wi-Fi screen once joined or skipped (from the LVGL task, outside key handling)
static void wifi_done(void) {
    finish();
}

static void advance(void *arg) {
    (void)arg;
    bool         have = shell_sources.wifi_has_credentials && shell_sources.wifi_has_credentials();
    setup_step_t next = setup_next(SETUP_WELCOME, have);
    if (next == SETUP_WIFI) {
        wifi_screen_open_setup(wifi_done);
    } else {
        finish();
    }
}

static void on_key(lv_event_t *e) {
    if (lv_event_get_key(e) == LV_KEY_ENTER) {
        lv_async_call(advance, NULL);
    }
}

static lv_obj_t *build_welcome(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    shell_style_screen(scr);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(scr, 20, 0);
    lv_obj_set_style_pad_hor(scr, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    logo_create(scr, 100);
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_label_set_text(title, "Welcome to Lichen");
    lv_obj_t *what = lv_label_create(scr);
    lv_obj_set_width(what, LV_PCT(90));
    lv_obj_set_style_text_align(what, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(what, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(what, "Your badge, and the apps that run on it.");
    lv_obj_t *hint = lv_label_create(scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_pad_top(hint, 24, 0);
    lv_label_set_text(hint, "Press Enter to start");
    lv_obj_add_event_cb(scr, on_key, LV_EVENT_KEY, NULL);
    return scr;
}

void setup_start(void) {
    if (setup_first(settings_get_u8("setup_done", 0) != 0) == SETUP_DONE) {
        return;
    }
    shell_open(build_welcome, SCREEN_WELCOME);
}

static void cmd_setup(char const *args) {
    if (strcmp(args, "reset") == 0) {
        bool ok = settings_set_u8("setup_done", 0); // only this key: Wi-Fi and the rest stay
        console_reply(ok ? "setup_done cleared: the welcome shows after the next reboot" : "could not clear setup_done");
        ESP_LOGW(TAG, "setup reset (%s)", ok ? "ok" : "failed");
    } else if (strncmp(args, "reset ", 6) == 0 || strncmp(args, "done ", 5) == 0) {
        // An app's first-run questions (#84 T5): the app keeps the flag itself; "mesh" stays as the short form. "done"
        // is a diagnostic
        bool        done = args[0] == 'd';
        char const *id   = args + (done ? 5 : 6);
        if (strcmp(id, "mesh") == 0) {
            id = "meshcore";
        }
        lichen_app_t const *app = lichen_app_by_id(id);
        if (!app || !app->first_run_set) {
            console_reply("no app '%s' with first-run questions", id);
            return;
        }
        app->first_run_set(done);
        if (done) {
            console_reply("%s: first-run questions marked answered", app->name);
        } else {
            console_reply("%s asks its first-run questions when next opened", app->name);
        }
        ESP_LOGW(TAG, "setup %s", args);
    } else {
        console_reply("setup reset [<app id>] | setup done <app id> (the first-run welcome, or an app's first-run questions; mesh = meshcore)");
    }
}

void setup_init(void) {
    console_register("setup", "setup reset (show the first-run welcome again after a reboot)", cmd_setup);
}
