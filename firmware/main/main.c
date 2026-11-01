/*
 * Lean badge firmware (#61): entry point. Modules are started here in order; for now it only reports itself.
 */

#include "backlight.h"
#include "board_i2c.h"
#include "boot_progress.h"
#include "c6link.h"
#include "crash.h"
#include "display.h"
#include "fw_console.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "log_store.h"
#include "psram_guard.h"
#include "log_tap.h"
#include "lora.h"
#include "mesh.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "lichen_sensors.h"
#include "lichen_app.h"
#include "lichen_random.h"
#include "nvs_flash.h"
#include "ota_server.h"
#include "ota_valid.h"
#include "settings_store.h"
#include "shell.h"
#include "assets.h"
#include "fonts.h"
#include "screenshot.h"
#include "sensors.h"
#include "sdcard.h"
#include "storage.h"
#include "update_mode.h"
#include "update_screen.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "badge_fw";

// What the status bar and About show, polled from the LVGL task: none of these waits on a lock
static bool wifi_up(void) {
    return wifi_has_ip(NULL, 0);
}

static bool lora_link_up(void) {
    return lora_up() && c6link_up();
}

// The OTA task's hooks, in the shell's terms (#76)
static void ota_begin(ota_kind_t kind, char const *name, uint32_t total) {
    if (kind != OTA_KIND_FILE) {
        psram_guard_update_begin(); // no-op when /prepare already added it (#40)
    }
    update_screen_begin(kind != OTA_KIND_FILE, kind == OTA_KIND_FIRMWARE, name, total);
}

static void ota_prepare(void) {
    update_screen_prepare();
    psram_guard_update_begin(); // an extra canary while update mode runs: #37 hit then (#40)
}

static void ota_end(bool ok, bool rebooting) {
    psram_guard_update_end();
    update_screen_end(ok, rebooting);
}

// What a firmware update stops at OS level (#76), after every app's stop_for_update: the LoRa radio, unless an app
// still runs (its lora_send must not meet a LoRa task that is going away, #84 T7), then the C6 link's update state
static void stop_for_update(bool apps_stopped) {
    if (apps_stopped) {
        lora_stop();
    } else {
        ESP_LOGE(TAG, "an app is still running: LoRa left as it is");
    }
    c6link_set_updating();
}

// ---- A C6 reset at runtime (#14 T-B): what the supervisor (c6sup) stops and restarts, in this order ----

static bool have_keyboard;

static void c6_down(void) {
    lora_c6_lost(); // sends fail at once, no polls until the C6 is back
}

// The re-attach pulsed C6 EN again, which also reset the TCA8418: the keyboard first, then the backlight (the C6 starts
// at its own defaults), then LoRa (config + RX start with MeshCore's settings)
static void c6_up(void) {
    if (have_keyboard) {
        keyboard_reinit();
    }
    backlight_reapply();
    lora_c6_back();
}

static void c6_restart_answer(bool confirmed) {
    c6link_fallback_answer(confirmed);
}

// Lucas 2026-10-31: a warning with a countdown, not a silent restart; the countdown ends in the restart because the
// keyboard may be dead in exactly this state
static bool c6_ask_restart(void) {
    shell_dialog_t const d = {
        .title        = "C6 (radio/Wi-Fi) not responding —",
        .count_prefix = "restarting in ",
        .count_suffix = " s.",
        .hint         = "Enter: restart now · Esc: stay without radio",
        .seconds      = 30,
    };
    return shell_dialog_countdown(&d, c6_restart_answer);
}

static void c6_wire_hooks(bool screen) {
    c6link_hooks_t const hooks = {
        .down        = c6_down,
        .up          = c6_up,
        .probe       = lora_c6_probe,
        .ask_restart = screen ? c6_ask_restart : NULL, // no screen: nobody to ask, the restart comes at once
        .ask_cancel  = screen ? shell_dialog_close : NULL,
    };
    c6link_set_hooks(&hooks);
    lora_set_send_hook(c6link_note_lora_send); // LoRa sends failing in a row: the soft signal
}

// The "apps" boot step (#66): every registered app's background part, on this task (core 0). FAILED names the apps
// that did not start; the others keep running.
static void start_apps(void) {
    boot_progress_begin(BOOT_APPS);
    char   failed[96] = ""; // boot_steps cuts the reason to BOOT_REASON_LEN on a character boundary
    size_t used       = 0;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (!app->start_background || app->start_background()) {
            continue;
        }
        ESP_LOGE(TAG, "%s not started", app->name);
        if (used < sizeof(failed)) {
            used += (size_t)snprintf(failed + used, sizeof(failed) - used, "%s%s", used ? ", " : "", app->name);
        }
    }
    if (used) {
        boot_progress_end(BOOT_APPS, STEP_FAILED, failed);
    } else {
        boot_progress_end(BOOT_APPS, STEP_DONE, NULL);
    }
}

static void log_heap(char const *name, uint32_t caps) {
    ESP_LOGI(
        TAG,
        "%s free %u largest %u",
        name,
        (unsigned)heap_caps_get_free_size(caps),
        (unsigned)heap_caps_get_largest_free_block(caps)
    );
}

void app_main(void) {
    lichen_random_start(); // the OS owns the entropy source and leaves it on (#84)
    log_tap_start();
    psram_guard_start(); // canaries early, so they sit low in the PSRAM heap (#40)
    log_store_start(); // after log_tap: its hook chains to the boot screen's
    console_set_tap(log_store_add);
    boot_progress_start();
    ESP_LOGI(TAG, "badge_fw %s starting", esp_app_get_description()->version);
    log_heap("default", MALLOC_CAP_DEFAULT);
    log_heap("internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    log_heap("dma", MALLOC_CAP_DMA);
    log_heap("psram", MALLOC_CAP_SPIRAM);
    boot_progress_begin(BOOT_PSRAM);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0) {
        char mb[16];
        snprintf(mb, sizeof(mb), "%u MB", (unsigned)(psram >> 20));
        boot_progress_end(BOOT_PSRAM, STEP_DONE, mb);
    } else {
        boot_progress_end(BOOT_PSRAM, STEP_FAILED, "no PSRAM");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Settings from an incompatible layout are lost rather than leaving the badge without settings at all
        ESP_LOGW(TAG, "NVS unusable (%s), erasing it", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
    }
    boot_progress_nvs_up();
    settings_apply_tz(); // before anything shows the clock
    boot_progress_begin(BOOT_STORAGE);
    if (!storage_mount()) {
        ESP_LOGE(TAG, "no storage drive");
        boot_progress_end(BOOT_STORAGE, STEP_FAILED, "no storage drive");
    } else {
        boot_progress_end(BOOT_STORAGE, STEP_DONE, NULL);
    }
    assets_mount(); // fonts; without them the UI uses the built-in ones (#22)
    crash_check();
    console_start();
    // The screen first, so the boot screen shows the slow C6 check and reflash. The backlight is the C6's (over the
    // UART the flasher uses) and the keyboard chip is reset with the C6: both come after it.
    boot_progress_begin(BOOT_DISPLAY);
    lv_display_t *display;
    bool          screen = display_start(&display);
    // The apps, in home's order: MeshCore first; shell_start adds Settings and About after it. Registered without a
    // screen too, so their background parts still start.
    meshcore_app_register();
    if (screen) {
        lichen_fonts_load(); // before any screen is built
    }
    assets_unmount(); // the fonts made their own PSRAM copies
    if (screen) {
        shell_sources_t const sources = {.wifi_up = wifi_up, .lora_up = lora_link_up, .c6_up = c6link_up,
                                         .wifi_scan = wifi_scan, .wifi_join = wifi_set_credentials, .wifi_ip = wifi_has_ip,
                                         .wifi_join_state = wifi_join_state,
                                         .wifi_has_credentials = wifi_has_credentials,
                                         .wifi_rssi = wifi_rssi, .wifi_joining = wifi_joining,
                                         .c6_trouble = c6link_trouble, .lora_counts = lora_get_counts};
        shell_set_sources(&sources);
        shell_start(display);
        boot_progress_end(BOOT_DISPLAY, STEP_DONE, NULL);
    } else {
        ESP_LOGE(TAG, "no display");
        boot_progress_end(BOOT_DISPLAY, STEP_FAILED, "no display");
    }
    if (screen) {
        // A restart leaves the backlight off (#23): once the boot screen has been drawn, turn it on again, before the
        // slow C6 check
        lvgl_port_lock(0);
        lv_refr_now(display);
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    backlight_restore();
    // The SD card (#60): before the C6 link, as BadgeVMS, so the SDMMC controller is set up before esp_hosted shares
    // it, and before the apps start. No card is one warning; nothing else waits for it.
    sd_start();
    if (!c6link_start()) {
        ESP_LOGE(TAG, "no C6 link: no Wi-Fi, no LoRa");
    }
    backlight_start();
    board_i2c_register_console();
    if (screen) {
        lv_indev_t *keys = NULL; // the keypad is an LVGL input device: LVGL first
        if (keyboard_start(&keys)) {
            shell_attach_keys(keys);
            keyboard_set_reset_hook(c6link_note_keyboard_reset); // the chip resets only with the C6 (#14)
            have_keyboard = true;
        } else {
            ESP_LOGE(TAG, "no keyboard");
        }
    }
    // The sensors (#69) share the keyboard's bus: after it. Their own task initialises the chips (the BMI270's 8 KB
    // config upload takes a while), so boot does not wait for them
    lichen_sensors_start();
    sensors_register_console();
    if (c6link_up()) {
        boot_progress_begin(BOOT_LORA);
        bool lora_ok = lora_start();
        if (!lora_ok) {
            ESP_LOGE(TAG, "LoRa not started");
        }
        boot_progress_end(BOOT_LORA, lora_ok ? STEP_DONE : STEP_FAILED, lora_ok ? NULL : "LoRa not started");
    } else {
        boot_progress_end(BOOT_LORA, STEP_FAILED, "no C6 link");
    }
    c6_wire_hooks(screen); // after keyboard and LoRa: from now on a C6 reset is recovered from
    start_apps();
    // Wi-Fi console and log (#38) work without a screen; the update screen hooks need one
    ota_hooks_t hooks = {.log_read = log_store_read};
#if CONFIG_OTA_WIFI_CONSOLE
    hooks.console_run = console_exec;
#endif
    if (screen) {
        hooks.begin = ota_begin;
        hooks.progress = update_screen_progress;
        hooks.end = ota_end;
        hooks.prepare = ota_prepare;
        hooks.file_ok = update_screen_file_ok;
        hooks.screenshot = screenshot_take;
        hooks.screenshot_free = screenshot_free;
        update_mode_set_stop_hook(stop_for_update);
        update_screen_boot_check();
    }
    ota_server_set_hooks(&hooks);
    ota_server_start();

    // A freshly installed image runs on trial until it has been up for a while; a reset before that boots the
    // previous one again
    esp_ota_img_states_t state;
    bool                 pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    while (pending) {
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (ota_should_mark_valid(pending, shell_up(), c6link_up(), uptime_s)) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGW(TAG, "this image is now valid");
            update_screen_image_valid();
            pending = false;
        } else if (ota_should_roll_back_now(pending, c6link_up(), uptime_s)) {
            // No C6 means no Wi-Fi, so nobody can replace this image: let the bootloader roll back
            ESP_LOGE(TAG, "C6 link never came up on a trial image: restarting to roll back");
            system_restart("trial image: no C6 link");
        } else if (uptime_s >= OTA_VALID_AFTER_S) {
            ESP_LOGE(TAG, "staying on trial (UI or C6 link down): a reset goes back to the previous image");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
