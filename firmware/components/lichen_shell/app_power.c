// Power (#65): Restart and Power off, a home tile and a row in Settings, each behind a confirm. Both stop the apps the
// way a firmware update does (they save what must survive), save the notifications and unmount the SD card; then the
// OS hook (main.c) restarts, or switches the backlight, the panel and the C6 off and puts the P4 in deep sleep. With
// the load that low the IP5306 cuts the battery after ~32 s and the power button starts the badge again. On USB power
// the IP5306 never cuts, and nothing the P4 can see wakes it: unplug, then press the power button.
#include "app_icons.h"
#include "esp_log.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "notify_os.h"
#include "menu.h"
#include "sdcard.h"
#include "shell.h"
#include "shell_parts.h"
#include "theme.h"
#include "update_mode.h"

#include <stdatomic.h>
#include <string.h>

#define TAG              "power"
#define SCREEN_POWER_BYE 151
#define CONFIRM_S        10
#define NOTIFY_SAVE_MS   2000
#define BYE_MIN_MS       3000 // the goodbye screen stays readable this long before the backlight goes
#define POWER_STACK      6144

static void (*finish_hook)(bool power_off);
static atomic_bool running;
static bool        want_off; // set before the task starts

bool power_going_down(void) {
    return atomic_load(&running);
}

void shell_set_power_hook(void (*finish)(bool power_off)) {
    finish_hook = finish;
}

static void power_task(void *arg) {
    (void)arg;
    bool       off   = want_off;
    TickType_t begin = xTaskGetTickCount();
    ESP_LOGW(TAG, "%s: stopping the apps", off ? "power off" : "restart");
    bool all_stopped = shell_stop_apps();
    update_mode_run_stop_hook(all_stopped); // LoRa (when every app stopped), the C6 link's update state
    if (!lichen_notify_save_now(NOTIFY_SAVE_MS)) {
        ESP_LOGW(TAG, "notifications not saved in time");
    }
    if (sd_mounted()) {
        sd_unmount();
    }
    TickType_t shown = xTaskGetTickCount() - begin;
    if (off && shown < pdMS_TO_TICKS(BYE_MIN_MS)) {
        vTaskDelay(pdMS_TO_TICKS(BYE_MIN_MS) - shown);
    }
    if (finish_hook) {
        finish_hook(off); // does not return
    }
    system_restart(off ? "power off (no hook)" : "power menu restart");
}

static bool eat_esc(void *arg) {
    (void)arg;
    return true; // nothing to go back to: the badge is going down
}

static void add_line(lv_obj_t *screen, char const *text, uint32_t colour, bool large) {
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(colour), 0);
    if (large) {
        lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_LARGE), 0);
    }
    lv_label_set_text(label, text);
}

static lv_obj_t *build_bye(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 24, 0);
    shell_on_esc(eat_esc, NULL);
    if (want_off) {
        add_line(screen, "Switching off", THEME_TEXT, true);
        add_line(screen, "Saving, then the screen goes dark.", THEME_DIM, false);
        add_line(screen, "On battery the badge switches itself off after half a minute. Press the power button to "
                         "start it again.", THEME_TEXT, false);
        add_line(screen, "On USB power it stays asleep: unplug it, then press the power button.", THEME_AMBER, false);
    } else {
        add_line(screen, "Restarting", THEME_TEXT, true);
        add_line(screen, "Saving first.", THEME_DIM, false);
    }
    return screen;
}

static void start(bool off) {
    if (atomic_exchange(&running, true)) {
        return;
    }
    want_off = off;
    shell_open(build_bye, SCREEN_POWER_BYE);
    if (xTaskCreatePinnedToCore(power_task, "power", POWER_STACK, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the power task: restarting as is");
        system_restart("power menu (no task)");
    }
}

static void answer_restart(bool yes) {
    if (yes) {
        start(false);
    }
}

static void answer_off(bool yes) {
    if (yes) {
        start(true);
    }
}

static void ask_restart(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title        = "Restart the badge?",
        .count_prefix = "(closes in ",
        .count_suffix = " s)",
        .hint         = "Enter: restart \xE2\x80\xA2 Esc: cancel",
        .seconds      = CONFIRM_S,
        .careful      = true,
    };
    shell_dialog_countdown(&d, answer_restart);
}

static void ask_off(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title        = "Switch the badge off?",
        .count_prefix = "(closes in ",
        .count_suffix = " s)",
        .hint         = "Enter: switch off \xE2\x80\xA2 Esc: cancel",
        .seconds      = CONFIRM_S,
        .careful      = true,
    };
    shell_dialog_countdown(&d, answer_off);
}

static void open_power(lv_obj_t *screen) {
    lv_obj_t *menu = menu_create(screen, "Power");
    menu_add(menu, "Restart", "", ask_restart, NULL);
    menu_add(menu, "Power off", "", ask_off, NULL);
    menu_note(menu, "Power off saves everything, then switches the screen, radio and processors off. On battery the "
                    "power button starts the badge again; on USB power unplug it first.");
}

static void cmd_poweroff(char const *args) {
    (void)args;
    if (atomic_load(&running)) {
        console_reply("already going down");
        return;
    }
    console_reply("switching off");
    start(true); // shell_open takes the LVGL lock itself
}

static lichen_app_t const power_app = {
    .name      = "Power",
    .id        = "power",
    .draw_icon = icon_power,
    .open      = open_power,
};

void power_app_register(void) {
    lichen_register_app(&power_app);
    console_register("poweroff", "poweroff (save, then switch the badge off as Power > Power off does)", cmd_poweroff);
}

void power_open_from_settings(void *arg) {
    (void)arg;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        if (lichen_app_at(i) == &power_app) {
            shell_open_app(i);
            return;
        }
    }
}
