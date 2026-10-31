// First-run setup (#66): a welcome after the first boot, then the Wi-Fi screen with a Skip row when the badge has no
// network yet. Lives on top of home on the shell's stack, so Esc from Wi-Fi lands on the welcome. setup_done is saved
// only when the setup ends (joined, skipped, or Enter on the welcome of a badge that already has Wi-Fi).
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fw_console.h"
#include "logo.h"
#include "settings_store.h"
#include "setup_state.h"
#include "shell_parts.h"
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
    lv_obj_set_style_text_font(title, &lv_font_montserrat_40, 0);
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
    } else if (strcmp(args, "reset mesh") == 0 || strcmp(args, "done mesh") == 0) {
        // MeshCore's first-open questions (#67): only their flag, the name and the radio stay. "done" is a diagnostic
        bool done = args[0] == 'd';
        bool ok   = settings_set_u8("mc_name_set", done ? 1 : 0);
        console_reply(ok ? (done ? "mc_name_set saved: MeshCore opens straight to Messages"
                                 : "mc_name_set cleared: MeshCore asks name and region when next opened")
                         : "could not change mc_name_set");
        ESP_LOGW(TAG, "setup %s (%s)", args, ok ? "ok" : "failed");
    } else {
        console_reply("setup reset [mesh] | setup done mesh (the first-run welcome, or MeshCore's name and region questions)");
    }
}

void setup_init(void) {
    console_register("setup", "setup reset (show the first-run welcome again after a reboot)", cmd_setup);
}
