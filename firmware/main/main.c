/*
 * Lean badge firmware (#61): entry point. app_main walks boot_order.c (#210) and runs one start function per entry.
 */

#include "backlight.h"
#include "lichen_sys_power.h"
#include "board_i2c.h"
#include "boot_order.h"
#include "boot_progress.h"
#include "c6link.h"
#include "wifi_service.h"
#include "crash.h"
#include "display.h"
#include "factory_reset.h"
#include "fw_console.h"
#include "esp_app_desc.h"
#include "lichen_sys_mem.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "log_store.h"
#include "psram_boot.h"
#include "psram_speed.h"
#include "psram_guard.h"
#include "log_tap.h"
#include "lora_os.h"
#include "lichen_crashlog.h"
#include "lichen_radio.h"
#include "lichen_apps.h"
#include "mesh.h"
#include "meshcore_run.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keyboard.h"
#include "lichen_sensors.h"
#include "haptics.h"
#include "led.h"
#include "lichen_app.h"
#include "lichen_mem.h"
#include "lichen_task.h"
#include "lichen_random.h"
#include "notify_os.h"
#include "lichen_nvs.h"
#include "nvs_erase_os.h"
#include "nvs_flash.h"
#include "ota_server.h"
#include "ota_valid.h"
#include "lichen_os.h"
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
#include <stdlib.h>
#include <string.h>

static char const TAG[] = "badge_fw";

// What the status bar and About show, polled from the LVGL task: none of these waits on a lock
static bool wifi_up(void) {
    return wifi_has_ip(NULL, 0);
}

static bool lora_link_up(void) {
    return lora_up() && c6link_up();
}

// MeshCore's run state in the status bar's terms (#103, #105)
static lora_use_t lora_use(void) {
    if (!lichen_radio_get(RADIO_LORA)) {
        return LORA_USE_OFF; // the OS radio switch or airplane mode (#108): off by choice, like a stopped MeshCore
    }
    mesh_run_t r = meshcore_run_get();
    if (!mesh_run_wants_radio(r)) {
        return LORA_USE_OFF;
    }
    if (mesh_run_busy(r)) {
        return LORA_USE_CHANGING;
    }
    return r.state == MESH_RUN_RUNNING ? LORA_USE_RUNNING : LORA_USE_FAILED;
}

// The LoRa spectrum (#161) borrows the radio: MeshCore stops, which gives the radio back (#103), and starts again
// afterwards. From the spectrum's own task. MeshCore must never stay off because of it: resume waits out a late stop
// (meshcore_run keeps STOPPING up to its LATE_STOP_MS, 60 s) before it starts MeshCore.
#define USER_PAUSE_WAIT_MS  20000
#define USER_RESUME_WAIT_MS 75000

static bool mesh_updating(void) {
    mesh_run_t r = meshcore_run_get();
    return r.state == MESH_RUN_STOPPED && r.why == MESH_WHY_UPDATE;
}

// No start or stop under way. False after wait_ms, when keep_going (may be NULL) says stop waiting, or during an update
static bool meshcore_settled(uint32_t wait_ms, bool (*keep_going)(void)) {
    for (uint32_t waited = 0;; waited += 100) {
        if (!mesh_run_busy(meshcore_run_get())) {
            return true;
        }
        if (waited >= wait_ms || (keep_going && !keep_going()) || mesh_updating()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static bool lora_user_pause(bool *was_running, bool (*keep_going)(void)) {
    *was_running = false;
    if (!meshcore_settled(USER_PAUSE_WAIT_MS, keep_going)) {
        return false;
    }
    if (meshcore_run_get().state != MESH_RUN_RUNNING) {
        return true;
    }
    meshcore_run_set_paused(true); // the screens say "Paused", not "Not running"; a start is refused with the reason
    char const *why = meshcore_run_request(false);
    if (why) {
        meshcore_run_set_paused(false);
        ESP_LOGW(TAG, "MeshCore not paused for the LoRa spectrum: %s", why);
        return false;
    }
    *was_running = true; // asked to stop: resume follows even if this stop is late
    if (meshcore_settled(USER_PAUSE_WAIT_MS, keep_going) && meshcore_run_get().state == MESH_RUN_STOPPED) {
        return true;
    }
    meshcore_run_set_paused(false); // not stopped: nothing is paused (a late stop is finished by resume)
    return false;
}

static void lora_user_resume_start(void) {
    for (int attempt = 1; attempt <= 2; attempt++) {
        if (!meshcore_settled(USER_RESUME_WAIT_MS, NULL)) {
            ESP_LOGW(TAG, "MeshCore %s: not started again after the LoRa spectrum",
                     mesh_updating() ? "stopped for an update" : "still busy");
            return;
        }
        mesh_run_t r = meshcore_run_get();
        if (r.state == MESH_RUN_RUNNING) {
            ESP_LOGI(TAG, "MeshCore runs again after the LoRa spectrum");
            return;
        }
        if (r.why == MESH_WHY_UPDATE) {
            ESP_LOGI(TAG, "MeshCore stopped for an update: not started again after the LoRa spectrum");
            return;
        }
        char const *why = meshcore_run_request(true);
        if (!why) {
            ESP_LOGI(TAG, "MeshCore starting again after the LoRa spectrum");
            return;
        }
        ESP_LOGW(TAG, "MeshCore start after the LoRa spectrum refused (%s)%s", why, attempt == 1 ? ": trying again" : "");
    }
}

static void lora_user_resume(void) {
    meshcore_run_set_paused(false); // a start is refused while paused; every way out of the resume leaves it cleared
    lora_user_resume_start();
}

// The OS radio switches (#108) reach the hardware here: the services own the radios, everyone else reads the state
static void radio_changed(radio_id_t id, bool on) {
    if (id == RADIO_WIFI) {
        wifi_enable(on);
    } else if (id == RADIO_LORA) {
        lora_set_enabled(on);
    } // RADIO_BT: no Bluetooth service before #73
}

// The OTA task's hooks, in the shell's terms (#76)
static void ota_begin(ota_kind_t kind, char const *name, uint32_t total) {
    if (kind != OTA_KIND_FILE) {
        psram_guard_update_begin(); // no-op when /prepare already added it (#40)
    }
    update_screen_begin(kind != OTA_KIND_FILE, kind == OTA_KIND_FIRMWARE, name, total);
    led_set_updating(true); // fast blink on the status LED (#74)
    haptics_set_updating(true); // no buzzing while the image arrives (#68)
}

static void ota_prepare(void) {
    update_screen_prepare();
    psram_guard_update_begin(); // an extra canary while update mode runs: #37 hit then (#40)
}

static void ota_end(bool ok, bool rebooting) {
    psram_guard_update_end();
    led_set_updating(false);
    haptics_set_updating(false);
    update_screen_end(ok, rebooting);
}

// What a firmware update stops at OS level (#76), after every app's stop_for_update: the LoRa radio, unless an app
// still runs (its lora_send must not meet a LoRa task that is going away, #84 T7), then the C6 link's update state
static void stop_for_update(bool apps_stopped) {
    if (apps_stopped) {
        lora_stop();
    } else {
        ESP_LOGW(TAG, "an app is still running: LoRa left as it is");
    }
    c6link_set_updating();
}

// Power off / Restart from the Power app (#65), after the apps stopped and the saves are done. Off: the backlights and
// the panel (the C6 drives the backlights, so first), then the C6 held in reset, then the P4 in deep sleep with nothing
// to wake it. That leaves the IP5306 under its ~50 mA load, so on battery it cuts the 5 V after ~32 s; the power
// button starts the badge from cold. Restart is the usual one (its shutdown hook blanks the screen).
static void power_finish(bool power_off) {
    if (!power_off) {
        system_restart("power menu restart");
    }
    system_restart_note("power off");
    haptics_stop();
    lichen_sys_power_screen_off();
    c6link_hold_in_reset();
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    ESP_LOGI(TAG, "power off: deep sleep, no wake source");
    esp_deep_sleep_start();
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

static uint32_t c6_restarts(char *reason, size_t cap) {
    static c6_status_t st; // the reason is long; only the sys_notices task calls this
    c6link_status(&st);
    snprintf(reason, cap, "%s", st.reason);
    return st.restarts;
}

static void c6_restart_answer(bool confirmed) {
    c6link_fallback_answer(confirmed);
}

// Lucas 2026-10-31: a warning with a countdown, not a silent restart; the countdown ends in the restart because the
// keyboard may be dead in exactly this state
static bool c6_ask_restart(void) {
    shell_dialog_t const d = {
        .title         = "The radio chip (Wi-Fi and LoRa) is not responding.",
        .confirm_label = "restart",
        .cancel_label  = "stay without radio",
        .seconds       = 30,
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
    // The plain apps (#180) join after the shell's own, so the default home order keeps them behind Settings, About and
    // Files; home is built when boot ends, and a saved order is merged by tile_order_merge
    lichen_apps_register();
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

static void log_heap(char const *name, lichen_sys_mem_kind_t kind) {
    ESP_LOGI(
        TAG,
        "%s free %u largest %u",
        name,
        (unsigned)lichen_sys_mem_free(kind),
        (unsigned)lichen_sys_mem_largest(kind)
    );
}

static uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// The first thing on the glass (#39): the boot screen, drawn before storage, notices, assets and the image hash. The
// fonts are LVGL's built-in ones until lichen_fonts_load. Returns false when there is no display.
static bool show_boot_screen(lv_display_t **display) {
    boot_progress_begin(BOOT_DISPLAY);
    if (!display_start(display)) {
        ESP_LOGE(TAG, "no display");
        boot_progress_end(BOOT_DISPLAY, STEP_FAILED, "no display");
        return false;
    }
    shell_boot_screen(*display);
    lichen_sys_ui_lock(0);
    lv_refr_now(*display);
    lichen_sys_ui_unlock();
    vTaskDelay(pdMS_TO_TICKS(40)); // the frame is on the panel before the backlight comes up
    ESP_LOGI(TAG, "first frame at %u ms", (unsigned)uptime_ms());
    boot_progress_end(BOOT_DISPLAY, STEP_DONE, NULL);
    // A restart switched the backlight off (#134); after a power-on the C6 comes up at its own default. This sets the
    // saved level. The same call closes the UART the C6 flasher needs for itself.
    backlight_restore();
    ESP_LOGI(TAG, "backlight on at %u ms", (unsigned)uptime_ms());
    return true;
}

// Fonts from the assets partition into PSRAM, then the assets copy goes (#22). After the screen exists: LVGL is running,
// so the lock is held while the fonts are parsed
static void load_fonts(bool screen) {
    assets_mount(); // without it the UI uses the built-in fonts
    if (screen) {
        lichen_sys_ui_lock(0);
        lichen_fonts_load(); // before shell_start builds the status bar and home
        lichen_sys_ui_unlock();
    }
    assets_unmount(); // the fonts made their own PSRAM copies
}

// NVS up before anything reads a setting (#116). nvs_boot_policy.h decides what each init result leads to: one erase
// on the plain path, none with a key burned.
static void nvs_boot(void) {
    esp_err_t       err  = lichen_nvs_init(); // nvs_flash_init(), or the encrypted path when a key is burned (#116)
    nvs_boot_step_t step = nvs_boot_next(lichen_nvs_boot_path(), err, false);
    if (step == NVS_BOOT_ERASE_RETRY) {
        // Plain path only: settings from an incompatible layout are lost rather than leaving the badge without any
        ESP_LOGW(TAG, "NVS unusable (%s), erasing it", esp_err_to_name(err));
        __real_nvs_flash_erase_partition(NVS_DEFAULT_PART_NAME); // the wrap refuses every other caller (#84 T9, #123)
        err  = lichen_nvs_init();
        step = nvs_boot_next(lichen_nvs_boot_path(), err, true);
    }
    if (step == NVS_BOOT_DOWN) {
        // With a key burned an erase could destroy what a later boot or a USB restore still reads: stay up without
        ESP_LOGE(TAG, "NVS init failed: %s%s", esp_err_to_name(err),
                 lichen_nvs_boot_path() == NVS_BOOT_PATH_KEYED
                     ? " (HMAC key burned: nothing erased, settings unavailable this boot)"
                     : "");
    }
    lichen_nvs_boot_done(step == NVS_BOOT_UP);
}

// ---- The boot steps (#210): one function per boot_order.h entry, run in boot_order.c's order ----

// What one step leaves for a later one
typedef struct {
    lv_display_t *display;
    bool          screen;     // a display came up
    bool          storage_ok; // /storage is mounted
} boot_ctx_t;

static void start_logs(boot_ctx_t *ctx) {
    (void)ctx;
    lichen_random_start(); // the OS owns the entropy source and leaves it on (#84)
    log_tap_start();
    psram_guard_start(); // canaries early, so they sit low in the PSRAM heap (#40)
    log_store_start(); // after log_tap: its hook chains to the boot screen's
    console_set_tap(log_store_add);
    boot_progress_start();
    ESP_LOGI(TAG, "badge_fw %s starting", esp_app_get_description()->version);
    log_heap("default", LICHEN_SYS_MEM_DEFAULT);
    log_heap("internal", LICHEN_SYS_MEM_INTERNAL);
    log_heap("dma", LICHEN_SYS_MEM_DMA);
    log_heap("psram", LICHEN_SYS_MEM_PSRAM);
}

static void start_psram(boot_ctx_t *ctx) {
    (void)ctx;
    boot_progress_begin(BOOT_PSRAM);
    size_t psram = lichen_sys_mem_total(LICHEN_SYS_MEM_PSRAM);
    if (psram > 0) {
        char mb[16];
        snprintf(mb, sizeof(mb), "%u MB", (unsigned)(psram >> 20));
        boot_progress_end(BOOT_PSRAM, STEP_DONE, mb);
    } else {
        boot_progress_end(BOOT_PSRAM, STEP_FAILED, "no PSRAM");
    }
}

static void start_nvs(boot_ctx_t *ctx) {
    (void)ctx;
    nvs_boot(); // #116: plain or encrypted, and never an erase with a key burned
    // Right after NVS is up and before anything can restart: the restart history (the "factory reset: ..." note, the PSRAM
    // restarts below) is written through this binding. It only stores function pointers (#235).
    settings_bind_services();
    factory_reset_boot_nvs(); // a pending factory reset (#66): before anything reads a setting
    psram_speed_boot(); // #121: the 80/120 MHz record: a failed 120 MHz trial falls back, a wanted 120 arms and restarts once
    psram_boot_oneshot_apply(); // #121: a one-shot delayline for the PSRAM bench, before storage and MeshCore load anything
    boot_progress_nvs_up();
}

static void start_early(boot_ctx_t *ctx) {
    (void)ctx;
    haptics_start();     // the motor pin low and the setting read, before anything slow (#68)
    settings_apply_tz(); // before anything shows the clock
    crash_check_early(); // cheap: only decides crash_recovered(), which the boot screen reads once
}

static void start_boot_screen(boot_ctx_t *ctx) {
    ctx->screen = show_boot_screen(&ctx->display);
}

static void start_console(boot_ctx_t *ctx) {
    (void)ctx;
    console_start();
}

static void start_storage(boot_ctx_t *ctx) {
    boot_progress_begin(BOOT_STORAGE);
    factory_reset_boot_storage(); // and its /storage part (#66): unmounted, before anything opens a file
    ctx->storage_ok = storage_mount();
    if (!ctx->storage_ok) {
        ESP_LOGE(TAG, "no storage drive");
        boot_progress_end(BOOT_STORAGE, STEP_FAILED, "no storage drive");
    } else {
        boot_progress_end(BOOT_STORAGE, STEP_DONE, NULL);
    }
}

static void start_notices(boot_ctx_t *ctx) {
    lichen_notify_start(ctx->storage_ok); // the history from /storage/lichen/notify.bin (#62); lichen_notify() works from now
    factory_reset_post_notices();         // a factory reset that could not erase from flash says so (#66)
    led_start();                          // the status LED follows the notices (#74)
    crash_check_late();                   // the crash record (/storage) and the full-image hash, after the first frame
    factory_reset_console_register();
    lichen_task_console_register(); // fw tasks (#181)
    lichen_mem_console_register();  // the per-app lines of fw mem (#181)
}

static void start_fonts(boot_ctx_t *ctx) {
    load_fonts(ctx->screen);
}

// The apps, in home's order: MeshCore first; shell_start adds Settings and About after it. Registered without a screen
// too, so their background parts still start.
static void start_shell(boot_ctx_t *ctx) {
    meshcore_app_register();
    system_restart_set_save(shell_save_for_restart); // a soft restart saves first, as an update does (#123)
    if (!ctx->screen) {
        return;
    }
    shell_sources_t const sources = {.wifi_up = wifi_up, .lora_up = lora_link_up, .c6_up = c6link_up,
                                     .lora_use = lora_use,
                                     .wifi_scan = wifi_scan, .wifi_join = wifi_set_credentials, .wifi_forget = wifi_forget_network,
                                     .wifi_saved_names = wifi_saved_names,
                                     .wifi_scan_raw = wifi_scan_raw, .wifi_ip = wifi_has_ip,
                                     .wifi_join_state = wifi_join_state,
                                     .wifi_has_credentials = wifi_has_credentials,
                                     .wifi_rssi = wifi_rssi, .wifi_joining = wifi_joining,
                                     .c6_trouble = c6link_trouble, .lora_counts = lora_get_counts,
                                     .c6_restarts = c6_restarts,
                                     .lora_user_pause = lora_user_pause, .lora_user_resume = lora_user_resume};
    shell_set_sources(&sources);
    shell_start(ctx->display);
    system_restart_set_prepare(shell_show_restarting); // a readable "Restarting" frame, then dark (#39, #134)
}

// The SD card (#60): before the C6 link, as BadgeVMS, so the SDMMC controller is set up before esp_hosted shares it,
// and before the apps start. No card is one warning; nothing else waits for it.
static void start_sd(boot_ctx_t *ctx) {
    (void)ctx;
    sd_start();
}

static void start_radio_switches(boot_ctx_t *ctx) {
    (void)ctx;
    lichen_radio_listen(radio_changed);
    lichen_radio_apply_all(); // the saved switches, before Wi-Fi and LoRa start
    lichen_radio_console_register();
}

// IDENT (#36): c6link asks the running C6 app through LoRa's peer data, before lora_start takes the callback over
static void start_lora_ident(boot_ctx_t *ctx) {
    (void)ctx;
    _Static_assert(C6LINK_IDENT_LEN == LORA_IDENT_LEN, "IDENT sha size");
    c6link_set_ident(lora_ident);
    c6link_set_ident_ready(lora_up);
}

// c6link (L2) reports its boot step through these (#223); lichen_boot has no business knowing the C6
_Static_assert(C6LINK_STEP_NOTE_LEN <= BOOT_REASON_LEN, "the C6 step note must fit the boot step's reason");

static void c6_step_begin(void) {
    boot_progress_begin(BOOT_C6);
}

static void c6_step_progress(int percent) {
    boot_progress_progress(BOOT_C6, percent);
}

static void c6_step_end(bool ok, char const *note) {
    boot_progress_end(BOOT_C6, ok ? STEP_DONE : STEP_FAILED, note);
}

// The Wi-Fi service (L3) may not call the gate's radio switch: `fw wifi on|off` reaches it through these
static bool wifi_radio_get(void) {
    return lichen_radio_get(RADIO_WIFI);
}

static bool wifi_radio_set(bool on) {
    return lichen_radio_set(RADIO_WIFI, on);
}

static void start_c6link(boot_ctx_t *ctx) {
    (void)ctx;
    c6link_boot_hooks_t const boot = {.begin = c6_step_begin, .progress = c6_step_progress, .end = c6_step_end};
    c6link_set_boot_hooks(&boot);
    wifi_radio_hooks_t const radio = {.get = wifi_radio_get, .set = wifi_radio_set, .airplane = lichen_airplane_get};
    settings_bind_net(); // the Wi-Fi credentials through the settings, passwords as secrets (#235): before any reader
    // Before c6link_start: it registers the net hooks, and c6link_start ends by bringing Wi-Fi up through them
    wifi_service_init(&radio);
    if (!c6link_start()) {
        ESP_LOGE(TAG, "no C6 link: no Wi-Fi, no LoRa");
    }
}

static void start_backlight(boot_ctx_t *ctx) {
    (void)ctx;
    backlight_start();
    board_i2c_register_console();
}

static void start_keyboard(boot_ctx_t *ctx) {
    if (!ctx->screen) {
        return;
    }
    lv_indev_t *keys = NULL; // the keypad is an LVGL input device: LVGL first
    bool        up   = keyboard_start(&keys);
    // keyboard_start was the last boot step to take the port lock directly (board): the lock-owner check starts now
    lichen_ui_service_arm();
    if (!up) {
        ESP_LOGE(TAG, "no keyboard");
        return;
    }
    shell_attach_keys(keys);
    keyboard_set_reset_hook(c6link_note_keyboard_reset); // the chip resets only with the C6 (#14)
    have_keyboard = true;
    factory_combo_start(); // Esc + Backspace held now: the settings-only reset for a badge whose UI is stuck
}

// The sensors (#69) share the keyboard's bus: after it. Their own task initialises the chips (the BMI270's 8 KB config
// upload takes a while), so boot does not wait for them
static void start_sensors(boot_ctx_t *ctx) {
    (void)ctx;
    if (sensors_boot_start()) {
        lichen_sensors_start();
    } else {
        ESP_LOGW(TAG, "sensors not started: fw sensors boot off");
    }
    sensors_register_console();
}

// "fw lora on|off" (#222): the OS switch saves it, and radio_changed hands it back to lora_set_enabled
static void lora_switch_requested(bool on) {
    lichen_radio_set(RADIO_LORA, on);
}

static void start_lora(boot_ctx_t *ctx) {
    (void)ctx;
    lora_set_switch_owner(lora_switch_requested); // before lora_start registers the console command
    if (!c6link_up()) {
        boot_progress_end(BOOT_LORA, STEP_FAILED, "no C6 link");
        return;
    }
    boot_progress_begin(BOOT_LORA);
    bool lora_ok = lora_start();
    if (!lora_ok) {
        ESP_LOGE(TAG, "LoRa not started");
    }
    boot_progress_end(BOOT_LORA, lora_ok ? STEP_DONE : STEP_FAILED, lora_ok ? NULL : "LoRa not started");
}

static void start_c6_hooks(boot_ctx_t *ctx) {
    c6_wire_hooks(ctx->screen); // after keyboard and LoRa: from now on a C6 reset is recovered from
}

static void start_apps_step(boot_ctx_t *ctx) {
    (void)ctx;
    start_apps();
}

static void start_ota(boot_ctx_t *ctx) {
    // Wi-Fi console and log (#38) work without a screen; the update screen hooks need one
    ota_hooks_t hooks = {.log_read = log_store_read, .crashes_read = lichen_crashlog_export};
#if CONFIG_OTA_WIFI_CONSOLE
    hooks.console_run = console_exec_with_secrets; // the reply goes only to the one who asked (#123)
#endif
    if (ctx->screen) {
        hooks.begin = ota_begin;
        hooks.progress = update_screen_progress;
        hooks.end = ota_end;
        hooks.prepare = ota_prepare;
        hooks.file_ok = update_screen_file_ok;
        hooks.screenshot = screenshot_take;
        hooks.screenshot_free = screenshot_free;
        update_mode_set_stop_hook(stop_for_update);
        shell_set_power_hook(power_finish);
        update_screen_boot_check();
    }
    ota_server_set_hooks(&hooks);
    ota_server_start();
}

typedef void (*boot_start_fn_t)(boot_ctx_t *ctx);

static boot_start_fn_t const start_fns[START_COUNT] = {
    [START_LOGS]           = start_logs,
    [START_PSRAM]          = start_psram,
    [START_NVS]            = start_nvs,
    [START_EARLY]          = start_early,
    [START_BOOT_SCREEN]    = start_boot_screen,
    [START_CONSOLE]        = start_console,
    [START_STORAGE]        = start_storage,
    [START_NOTICES]        = start_notices,
    [START_FONTS]          = start_fonts,
    [START_SHELL]          = start_shell,
    [START_SD]             = start_sd,
    [START_RADIO_SWITCHES] = start_radio_switches,
    [START_LORA_IDENT]     = start_lora_ident,
    [START_C6LINK]         = start_c6link,
    [START_BACKLIGHT]      = start_backlight,
    [START_KEYBOARD]       = start_keyboard,
    [START_SENSORS]        = start_sensors,
    [START_LORA]           = start_lora,
    [START_C6_HOOKS]       = start_c6_hooks,
    [START_APPS]           = start_apps_step,
    [START_OTA]            = start_ota,
};

// A freshly installed image runs on trial until it has been up for a while; a reset before that boots the previous one
// again
static void ota_trial_wait(void) {
    esp_ota_img_states_t state;
    bool                 pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;
    while (pending) {
        uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (ota_should_mark_valid(pending, shell_up(), c6link_up(), uptime_s)) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "this image is now valid");
            update_screen_image_valid();
            pending = false;
        } else if (ota_should_roll_back_now(pending, c6link_up(), uptime_s)) {
            // No C6 means no Wi-Fi, so nobody can replace this image: let the bootloader roll back
            ESP_LOGE(TAG, "C6 link never came up on a trial image: restarting to roll back");
            system_restart("trial image: no C6 link");
        } else if (uptime_s >= OTA_VALID_AFTER_S) {
            ESP_LOGW(TAG, "staying on trial (UI or C6 link down): a reset goes back to the previous image");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// A step added to boot_order.h without its start function would be a NULL call on every boot: say which, and stop
static void check_start_fns(void) {
    _Static_assert(sizeof(start_fns) / sizeof(start_fns[0]) == START_COUNT, "one start function slot per step");
    for (int s = 0; s < START_COUNT; s++) {
        if (!start_fns[s]) {
            ESP_LOGE(TAG, "boot step %s has no start function", boot_start_name((boot_start_t)s));
            abort();
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "app_main starts at %u ms of uptime (esp_timer, counted from reset)", (unsigned)uptime_ms());
    check_start_fns();
    boot_ctx_t ctx = {0};
    // boot_order.c is the order (the two-chip contract, host_tests/boot_order_test.c); this only walks it
    for (size_t i = 0; i < boot_order_len; i++) {
        start_fns[boot_order[i]](&ctx);
    }
    ota_trial_wait();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
