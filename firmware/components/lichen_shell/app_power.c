// Power (#65): Restart and Power off, a home tile and a row in Settings, each behind a confirm. Both stop the apps the
// way a firmware update does (they save what must survive), save the notifications and unmount the SD card; then the
// OS hook (main.c) restarts, or switches the backlight, the panel and the C6 off and puts the P4 in deep sleep. With
// the load that low the IP5306 cuts the battery after ~32 s and the power button starts the badge again. On USB power
// the IP5306 never cuts, and nothing the P4 can see wakes it: unplug, then press the power button.
#include "app_icons.h"
#include "screen_ids.h"
#include "esp_log.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "lichen_app.h"
#include "notify_os.h"
#include "lichen_task.h"
#include "menu.h"
#include "lichen_sys_storage.h"
#include "shell.h"
#include "shell_parts.h"
#include "theme.h"
#include "update_mode.h"

#include <stdatomic.h>
#include <string.h>

#define TAG              "power"
#define CONFIRM_S        10
#define NOTIFY_SAVE_MS   2000
#define BYE_MIN_MS       3000 // the goodbye screen stays readable this long before the backlight goes
#define POWER_STACK      6144
#define BYE_UI_STACK     6144 // builds the goodbye screen

static lichen_app_t const power_app; // its tasks are booked under it (#181)
static void (*finish_hook)(bool power_off);
static atomic_bool running;
static atomic_bool want_off; // set before the task starts; a power off asked meanwhile turns a restart into one (#281)

bool power_going_down(void) {
    return atomic_load(&running);
}

void shell_set_power_hook(void (*finish)(bool power_off)) {
    finish_hook = finish;
}

static void power_task(void *arg) {
    (void)arg;
    bool       off   = atomic_load(&want_off);
    TickType_t begin = xTaskGetTickCount();
    ESP_LOGI(TAG, "%s: stopping the apps", off ? "power off" : "restart");
    bool all_stopped = shell_stop_apps();
    update_mode_run_stop_hook(all_stopped); // LoRa (when every app stopped), the C6 link's update state
    if (!lichen_notify_save_now(NOTIFY_SAVE_MS)) {
        ESP_LOGW(TAG, "notifications not saved in time");
    }
    if (lichen_sys_sd_mounted()) {
        lichen_sys_sd_unmount();
    }
    TickType_t shown = xTaskGetTickCount() - begin;
    if (off && shown < pdMS_TO_TICKS(BYE_MIN_MS)) {
        vTaskDelay(pdMS_TO_TICKS(BYE_MIN_MS) - shown);
    }
    off = atomic_load(&want_off); // the thermal shutdown may have turned this restart into a power off meanwhile
    if (finish_hook) {
        finish_hook(off); // does not return
    }
    system_restart_asked(off ? "power off (no hook)" : "power menu restart");
}

// The notifications first: a short write, so a mesh stop that runs into the restart's bound cannot cut their file
void shell_save_for_restart(void) {
    if (!lichen_notify_save_now(NOTIFY_SAVE_MS)) {
        ESP_LOGW(TAG, "notifications not saved in time");
    }
    ESP_LOGI(TAG, "restart: stopping the apps");
    if (!shell_stop_apps()) {
        ESP_LOGW(TAG, "restart: an app did not stop, its last changes may be lost");
    }
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
    if (atomic_load(&want_off)) {
        add_line(screen, "Powering off...", THEME_TEXT, true);
        add_line(screen, "Saving, then the screen goes dark.", THEME_DIM, false);
        add_line(screen, "On battery the badge switches itself off after half a minute. Press the power button to "
                         "start it again.", THEME_TEXT, false);
        add_line(screen, "On USB power it stays asleep: unplug it, then press the power button.", THEME_AMBER, false);
    } else {
        add_line(screen, "Restarting...", THEME_TEXT, true);
        add_line(screen, "Saving first.", THEME_DIM, false);
        if (shell_restart_note_get()[0]) {
            add_line(screen, shell_restart_note_get(), THEME_AMBER, false);
        }
    }
    return screen;
}

static void bye_task(void *arg) {
    (void)arg;
    shell_open(build_bye, SCREEN_POWER_BYE);
}

static void start(bool off) {
    if (atomic_exchange(&running, true)) {
        // Already going down: a power off still wins over a restart (the thermal shutdown must not end in a restart)
        if (off && !atomic_exchange(&want_off, true)) {
            ESP_LOGW(TAG, "power off asked during a restart: powering off instead");
        }
        return;
    }
    if (off) {
        atomic_store(&want_off, true); // only ever set: no restart path may clear a power off asked for meanwhile
    }
    // The task first: a busy or stuck LVGL lock must not hold up a (thermal) shutdown. The goodbye screen is opened by
    // a short task of its own, which may wait for the lock as long as it needs.
    if (!lichen_task_start(&power_app, "power", power_task, NULL, POWER_STACK, LICHEN_PRIO_URGENT, NULL)) {
        ESP_LOGE(TAG, "no memory for the power task: restarting as is");
        system_restart_asked("power menu (no task)");
    }
    if (!lichen_task_start(&power_app, "pwrui", bye_task, NULL, BYE_UI_STACK, LICHEN_PRIO_URGENT, NULL)) {
        ESP_LOGW(TAG, "no memory for the goodbye screen");
    }
}

void power_request_off(void) {
    start(true);
}

void power_request_restart(void) {
    start(false);
}

static void do_restart(void *arg) {
    (void)arg;
    start(false);
}

static void do_off(void *arg) {
    (void)arg;
    start(true);
}

static void open_power(lv_obj_t *screen) {
    lv_obj_t *menu = menu_create(screen, "Power");
    menu_set_danger(menu_add(menu, "Restart", "", do_restart, NULL), "Restart the badge?", "restart");
    menu_set_danger(menu_add(menu, "Power off", "", do_off, NULL), "Power off the badge?", "power off");
    menu_note(menu, "Power off saves everything, then switches the screen, radio and processors off. On battery the "
                    "power button starts the badge again; on USB power unplug it first.");
}

static lichen_app_t const power_app = {
    .name      = "Power",
    .id        = "power",
    .draw_icon = icon_power,
    .open      = open_power,
};

void power_app_register(void) {
    lichen_register_app(&power_app); // `fw poweroff` is main's (#292): it also works without a display
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
