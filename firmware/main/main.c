/*
 * Lean badge firmware (#61): entry point. app_main walks boot_order.c (#210) and runs one start function per entry.
 */

#include "backlight.h"
#include "battery.h"
#include "lichen_sys_power.h"
#include "board_i2c.h"
#include "boot_keys_play.h"
#include "boot_order.h"
#include "boot_progress.h"
#include "power_route.h"
#include "c6link.h"
#include "wifi_service.h"
#include "crash.h"
#include "display.h"
#include "factory_reset.h"
#include "fw_console.h"
#include "fw_slot.h"
#include "esp_app_desc.h"
#include "lichen_sys_mem.h"
#include "lichen_sys_net.h"
#include "esp_log.h"
#include "lichen_sys_storage.h"
#include "lichen_sys_ui.h"
#include "esp_timer.h"
#include "log_store.h"
#include "psram_boot.h"
#include "psram_speed.h"
#include "psram_guard.h"
#include "log_tap.h"
#include "lora_os.h"
#include "lichen_crashlog.h"
#include "lichen_radio.h"
#include "lichen_sys_notify.h"
#include "lichen_apps.h"
#include "mesh.h"
#include "meshcore_run.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "keyboard.h"
#include "lichen_sensors.h"
#include "haptics.h"
#include "led.h"
#include "lichen_app.h"
#include "app_ns.h"
#include "factory_plan.h"
#include "lichen_sys_app.h"
#include "lichen_mem.h"
#include "lichen_task.h"
#include "lichen_random.h"
#include "lichen_clock.h"
#include "notify_os.h"
#include "lichen_nvs.h"
#include "nvs_erase_os.h"
#include "nvs_flash.h"
#include "ota_server.h"
#include "ota_os.h"
#include "ota_trial.h"
#include "ota_valid.h"
#include "lichen_os.h"
#include "safe_mode.h"
#include "safe_mode_policy.h"
#include "settings_store.h"
#include "shell.h"
#include "soak_cmd.h"
#include "assets.h"
#include "fonts.h"
#include "screenshot.h"
#include "sensors.h"
#include "sdcard.h"
#include "storage.h"
#include "update_mode.h"
#include "update_screen.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const TAG[] = "badge_fw";

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
    char const *why = meshcore_run_request_system(false); // the OS, not a key (#400)
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
        char const *why = meshcore_run_request_system(true); // puts back what ran (#400)
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

// Power off / Restart from the Power app (#65), after the apps stopped and the saves are done. Off is
// lichen_sys_power_off_now (lichen_os since #281): backlights, panel, C6 held in reset, P4 in deep sleep; on battery the
// IP5306 cuts the 5 V after ~32 s. Restart is the usual one (its shutdown hook blanks the screen).
static void power_finish(bool power_off) {
    if (!power_off) {
        system_restart_asked("power menu restart");
    }
    lichen_sys_power_off_now();
}

// The SD card's share of a headless power off, bounded like the saves: a stuck card must not keep a hot badge on
#define SD_UNMOUNT_MS 2000
static StaticSemaphore_t sd_unmounted_buf;
static SemaphoreHandle_t sd_unmounted;

static void sd_unmount_task(void *arg) {
    (void)arg;
    lichen_sys_sd_unmount();
    xSemaphoreGive(sd_unmounted);
}

static void sd_unmount_bounded(void) {
    if (!lichen_sys_sd_mounted()) {
        return;
    }
    sd_unmounted = xSemaphoreCreateBinaryStatic(&sd_unmounted_buf);
    if (!lichen_task_start(lichen_sys_ctx_os(), "sdoff", sd_unmount_task, NULL, 4096, LICHEN_PRIO_URGENT, NULL) ||
        xSemaphoreTake(sd_unmounted, pdMS_TO_TICKS(SD_UNMOUNT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "power off: the SD card was not unmounted in time, going off anyway");
    }
}

// A power off without the shell (#281, #292: no display): the saves the Power app would run (notifications, the apps
// stop, bounded as for a restart), the SD card (bounded), then off. Never returns.
static atomic_bool headless_going_down;

static void power_off_headless(void) {
    ESP_LOGW(TAG, "power off without the shell: saving, then off");
    system_save_before_off();
    sd_unmount_bounded();
    lichen_sys_power_off_now();
}

// power_route.c decides (#292, host_tests/power_route_test.c); false when a power off is already under way
static bool power_off_by_route(bool thermal) {
    bool up = shell_up();
    switch (power_route(up, up && power_going_down(), atomic_load(&headless_going_down), thermal)) {
    case POWER_ROUTE_SHELL:
        system_power_off_requested(lichen_sys_power_off_now); // a restart from anywhere now ends in the power off
        power_request_off(); // during a restart this turns it into a power off
        return true;
    case POWER_ROUTE_HEADLESS:
        if (atomic_exchange(&headless_going_down, true)) {
            return false;
        }
        // A fw reboot or an update racing this one (its saves wait for ours) powers off too, never restarts
        system_power_off_requested(lichen_sys_power_off_now);
        power_off_headless();
        return true;
    case POWER_ROUTE_BUSY:
    default:
        return false;
    }
}

// The thermal watchdog's power off
static void thermal_power_off(void) {
    power_off_by_route(true);
}

// `fw poweroff` (#65, #292): main's, so it powers off without a display too
static void cmd_poweroff(char const *args) {
    (void)args;
    bool headless = !shell_up();
    if (power_route(!headless, !headless && power_going_down(), atomic_load(&headless_going_down), false) ==
        POWER_ROUTE_BUSY) {
        console_reply("already going down");
        return;
    }
    console_reply(headless ? "powering off (no display: saving, then off)" : "powering off");
    fflush(stdout);
    power_off_by_route(false);
}

// The headless watch services settle on this (#281): every boot step has run
static atomic_bool boot_steps_done;

static bool boot_finished(void) {
    return atomic_load(&boot_steps_done);
}

static int boot_update_result(char *line, size_t cap) {
    return (int)update_screen_boot_result(line, cap);
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
        // #232: crashed twice in three boots (logged by safe_mode.c); asked for every app, so the rule sees which
        // ones have a background part it may switch off (never an exempt one: MeshCore)
        bool may_off = safe_mode_may_switch_off(app->start_background != NULL, app->safe_mode_exempt);
        if (safe_mode_background_off(app->id, may_off) || !app->start_background) {
            continue;
        }
        lichen_ctx_t previous = lichen_ctx_enter(lichen_sys_app_ctx_at(i)); // #230: its tasks inherit the app
        bool         started  = app->start_background();
        lichen_ctx_leave(previous);
        if (started) {
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
// fonts are LVGL's built-in ones until lichen_fonts_load. Returns whether the display is up, skipped or failed.
#define NOSCREEN_KEY "t_noscreen" // `fw noscreen`: the next boot acts as if the display did not come up (#281)

static ota_display_t show_boot_screen(lv_display_t **display) {
    boot_progress_begin(BOOT_DISPLAY);
    // One boot only: erased before anything else can go wrong. A key that cannot be erased would keep every boot
    // screenless, so it is then ignored and the display starts.
    if (settings_get_u8(NOSCREEN_KEY, 0)) {
        if (!settings_erase(NOSCREEN_KEY)) {
            ESP_LOGW(TAG, "fw noscreen key could not be erased: ignored, the display starts");
        } else {
            ESP_LOGW(TAG, "display start skipped on purpose (fw noscreen, this boot only)");
            boot_progress_end(BOOT_DISPLAY, STEP_SKIPPED, "fw noscreen");
            return OTA_DISPLAY_SKIPPED;
        }
    }
    if (!display_start(display)) {
        ESP_LOGE(TAG, "no display");
        boot_progress_end(BOOT_DISPLAY, STEP_FAILED, "no display");
        return OTA_DISPLAY_FAILED;
    }
    lichen_ui_service_port_up();
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
    return OTA_DISPLAY_UP;
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
    ota_display_t display_is; // up, skipped on purpose (fw noscreen) or failed: the OTA trial tells them apart (#293)
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

// The keys apps want kept over a factory reset (lichen_app_t.keep_keys, #230 E1d): the reset runs at NVS start, so the
// apps that have them are registered before it
static void declare_keep_keys(void) {
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app    = lichen_app_at(i);
        size_t              failed = app_ns_declare_keep(app->id, app->secret_prefix, app->keep_keys, factory_keep_app_key);
        if (failed) {
            ESP_LOGE(TAG, "%s: %u key(s) not kept over a factory reset", app->name, (unsigned)failed);
        }
    }
}

// The apps' plain secrets into their secrets class (#288): at every boot, a no-op once none are left; one line with the
// counts when there was work, never a key or a value. Not in a boot that migrated NVS to encrypted (#116): one
// settings migration per boot.
static void move_app_secrets(void) {
    unsigned moved = 0, dropped = 0, failed = 0;
    bool     ran = false, deferred = false;
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const     *app = lichen_app_at(i);
        app_secret_move_count_t c;
        app_secret_move_t       r = app_secrets_move_app(app->id, app->secret_prefix, lichen_nvs_migrated(), &c);
        deferred                  = deferred || r == APP_SECRET_MOVE_DEFERRED;
        ran                       = ran || r == APP_SECRET_MOVE_DONE || r == APP_SECRET_MOVE_PARTIAL;
        moved += c.moved;
        dropped += c.dropped;
        failed += c.failed;
    }
    if (deferred) {
        ESP_LOGW(TAG, "app secrets not moved: NVS was migrated this boot (the next boot moves them)");
    }
    if (ran) {
        ESP_LOGI(TAG, "app secrets: %u moved into the secrets class, %u invalid dropped, %u not moved (retried next boot)",
                 moved, dropped, failed);
    }
}

// #353: the crash of a settled 120 MHz session went to the crash log (a failed write is logged at W there: the notice,
// which the restart saves, is then its trace) and its notice is raised: the re-arm may restart now
static bool crash_told(void) {
    return crash_record_state() != CRASH_RECORD_PENDING && lichen_watch_boot_told();
}

// #353 review: a speed restart must not cut a P4 OTA write, a file push (ota-c6 sends the C6 image as one) or the C6
// flasher
static bool update_busy(void) {
    return update_screen_busy() || c6link_updating() || c6link_flashing();
}

static void start_nvs(boot_ctx_t *ctx) {
    (void)ctx;
    nvs_boot(); // #116: plain or encrypted, and never an erase with a key burned
    // Right after NVS is up and before anything can restart: the restart history (the "factory reset: ..." note, the PSRAM
    // restarts below) is written through this binding. It only stores function pointers (#235).
    settings_bind_services();
    meshcore_app_register(); // first in home's order; its keep list is needed by the reset
    declare_keep_keys();
    factory_reset_boot_nvs(); // a pending factory reset (#66): before anything reads a setting
    move_app_secrets();       // #288: after the reset, before any app reads a setting
    settings_bind_dev_mode(); // reads dev.mode (#373): after the reset, which erases it
    // #121, #353: the 80/120 MHz record: bad bytes in the boot check at 120 fall back to 80, a wanted 120 arms and
    // restarts once (not while the image is on trial, #285: ota answers that; not after a crash, which crash.c's core
    // dump tells from this board's reason-7 power-on)
    static psram_speed_hooks_t const speed_hooks = {.image_on_trial = ota_image_trial_maybe, // unread = on trial
                                                    .dump_fresh     = crash_dump_fresh,
                                                    .crash_told     = crash_told,
                                                    .update_busy    = update_busy,
                                                    .rearm_mark     = safe_mode_rearm_mark};
    psram_speed_boot(&speed_hooks);
    psram_boot_oneshot_apply(); // #121: a one-shot delayline for the PSRAM bench, before storage and MeshCore load anything
    boot_progress_nvs_up();
}

static void start_early(boot_ctx_t *ctx) {
    (void)ctx;
    lichen_sys_mem_start(); // the gate's widgets allocate through lichen_alloc (#296): before the boot screen
    haptics_start();     // the motor pin low and the setting read, before anything slow (#68)
    settings_apply_tz(); // before anything shows the clock
    clock_boot_init();   // the carried or the saved clock (#326), before anything stamps a time
    crash_check_early(); // cheap: only decides crash_recovered(), which the boot screen reads once
    safe_mode_boot_step(crash_recovered()); // #232: the crash's app, 2 in 3 boots = its background off
}

static void start_boot_screen(boot_ctx_t *ctx) {
    ctx->display_is = show_boot_screen(&ctx->display);
    ctx->screen     = ctx->display_is == OTA_DISPLAY_UP;
}

// The bench's way to boot without a display (#281): proves the OS services that must not depend on the shell
static void cmd_noscreen(char const *args) {
    (void)args;
    if (!settings_set_u8(NOSCREEN_KEY, 1)) {
        console_reply("noscreen: not saved");
        return;
    }
    console_reply("noscreen: the next boot skips the display (that boot only; no screen, no keyboard). fw reboot now.");
}

static void start_console(boot_ctx_t *ctx) {
    (void)ctx;
    console_start();
    console_bind_slot_state(ota_slot_state_name); // "fw slot": ota owns the slot states (#285)
    console_bind_slot_switch(ota_image_note_slot_switch); // its trial needs no Wi-Fi (#411)
    soak_cmd_register(); // "fw soak" (#409), developer mode
    console_register("noscreen", "noscreen (test: the next boot skips the display start, one boot only)", cmd_noscreen);
    console_register("poweroff", "poweroff (save, then switch the badge off as Power > Power off does; works without a display)",
                     cmd_poweroff);
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
    lichen_sys_app_console_register(); // fw app ctx (#230)
    lichen_mem_console_register();  // the per-app lines of fw mem (#181)
}

static void start_fonts(boot_ctx_t *ctx) {
    load_fonts(ctx->screen);
}

// The apps, in home's order: MeshCore first; shell_start adds Settings and About after it. Registered without a screen
// too, so their background parts still start.
static void start_shell(boot_ctx_t *ctx) {
    system_restart_set_save(shell_save_for_restart); // a soft restart saves first, as an update does (#123)
    if (!ctx->screen) {
        return;
    }
    // The radio's borrowers (#161, lichen_radio.h) and what MeshCore does with it (the status bar's LoRa colour) are
    // MeshCore's (L6): everything else the shell shows comes through lichen_sys_*.
    lichen_radio_set_lora_user(lora_user_pause, lora_user_resume);
    shell_sources_t const sources = {.lora_use = lora_use};
    shell_set_sources(&sources);
    shell_start(ctx->display);
    // Before the watchdog starts (#281): a thermal shutdown early in the boot gets the real power off, not a restart
    shell_set_power_hook(power_finish);
    system_restart_set_prepare(shell_show_restarting); // a readable "Restarting" frame, then dark (#39, #134)
}

// The thermal watchdog and Lichen's own notices (#281): OS services, started with or without a display. Only the
// hooks into the shell depend on the screen.
static void start_watch(boot_ctx_t *ctx) {
    lichen_watch_hooks_t hooks = {.ui_ready = boot_finished, .power_off = thermal_power_off};
    if (ctx->screen) {
        hooks.ui_ready      = shell_home_shown;
        hooks.hot_alert     = battery_open_alert;
        hooks.update_result = boot_update_result;
    }
    lichen_watch_start(&hooks);
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

// The Wi-Fi saved-list copy lives in PSRAM, owner OS (#249)
static void *wifi_alloc_bulk(size_t size) {
    return lichen_alloc(lichen_sys_ctx_os(), size, LICHEN_MEM_BULK);
}
static void wifi_free_bulk(void *p) {
    lichen_free(lichen_sys_ctx_os(), p);
}

static void start_c6link(boot_ctx_t *ctx) {
    (void)ctx;
    c6link_boot_hooks_t const boot = {.begin          = c6_step_begin,
                                      .progress       = c6_step_progress,
                                      .end            = c6_step_end,
                                      .image_on_trial = ota_image_trial_maybe}; // an unread state: on trial
    c6link_set_boot_hooks(&boot);
    wifi_radio_hooks_t const radio = {.get = wifi_radio_get, .set = wifi_radio_set, .airplane = lichen_airplane_get};
    wifi_bind_alloc(wifi_alloc_bulk, wifi_free_bulk);
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
    // "fw boot keys" (#379): cleared first, on every path, so a crash while the keys are held does not hold them again
    // on every boot, and a boot without a screen or keyboard uses up the shot too
    boot_keys_t armed = boot_keys_take_armed();
    if (!ctx->screen) {
        boot_keys_skipped(armed, "no screen");
        return;
    }
    lv_indev_t *keys = NULL; // the keypad is an LVGL input device: LVGL first
    bool        up   = keyboard_start(&keys);
    // keyboard_start was the last boot step to take the port lock directly (board): the lock-owner check starts now
    lichen_ui_service_arm();
    if (!up) {
        ESP_LOGE(TAG, "no keyboard");
        boot_keys_skipped(armed, "no keyboard");
        return;
    }
    shell_attach_keys(keys);
    keyboard_set_reset_hook(c6link_note_keyboard_reset); // the chip resets only with the C6 (#14)
    have_keyboard = true;
    factory_combo_start(); // Esc + Backspace held now: the settings-only reset for a badge whose UI is stuck
    // After the combo's hook is in: the poll task (priority 4) handles an injected key at once, and a press sent
    // before the hook was added would never reach the combo
    boot_keys_play(armed);
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

// The fuel gauge (#11) on the camera header's bus, its own task: the probe runs there, so boot never waits for it. LP
// RAM stack (#269): it touches no flash, NVS or /storage and puts no DMA buffer on its stack.
static void battery_task(void *arg) {
    (void)arg;
    for (;;) {
        battery_service_run(); // never returns; the loop makes sure an LP RAM task cannot end (#269)
    }
}

static void start_battery(boot_ctx_t *ctx) {
    (void)ctx;
    battery_register_console();
    if (!lichen_task_start_in(lichen_sys_ctx_os(), "battery", battery_task, NULL, BATTERY_STACK_BYTES,
                              LICHEN_PRIO_BACKGROUND, LICHEN_STACK_LP_RAM, NULL)) {
        ESP_LOGE(TAG, "battery service not started");
    }
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
        lichen_reg_start(); // the P4's own keys; the C6's show "no link to the C6"
        return;
    }
    boot_progress_begin(BOOT_LORA);
    bool lora_ok = lora_start();
    if (!lora_ok) {
        ESP_LOGE(TAG, "LoRa not started");
    }
    lichen_reg_start(); // after lora_start: its first c6.reg_caps ask goes out through the LoRa task (#310)
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
    ota_hooks_t hooks = {
        .log_read = log_store_read, .crashes_read = lichen_crashlog_export, .hw_text = lichen_reg_cached_text};
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
    [START_WATCH]          = start_watch,
    [START_SD]             = start_sd,
    [START_RADIO_SWITCHES] = start_radio_switches,
    [START_LORA_IDENT]     = start_lora_ident,
    [START_C6LINK]         = start_c6link,
    [START_BACKLIGHT]      = start_backlight,
    [START_KEYBOARD]       = start_keyboard,
    [START_SENSORS]        = start_sensors,
    [START_BATTERY]        = start_battery,
    [START_LORA]           = start_lora,
    [START_C6_HOOKS]       = start_c6_hooks,
    [START_APPS]           = start_apps_step,
    [START_OTA]            = start_ota,
};

// The trial loop's inputs and actions (ota_trial_io_t): ota_valid.c runs the loop, this is its glue
static void trial_read(void *ctx, ota_trial_in_t *in) {
    in->display     = *(ota_display_t const *)ctx;
    in->image       = ota_image_trial_state();
    in->shell_up    = in->display == OTA_DISPLAY_UP && shell_up();
    in->c6_up       = c6link_up();
    in->uptime_s    = uptime_ms() / 1000;
    in->arrival     = ota_image_arrival(); // #411: the receiver's notion of Wi-Fi, the person's saved switch
    ota_wifi_seen_t const seen = {.test_no_wifi = ota_image_arrival_test_no_wifi(),
                                  .switch_on    = ota_os_wifi_switch_on(),
                                  .held         = ota_os_wifi_held(),
                                  .radio_on     = ota_os_wifi_on(),
                                  .has_ip       = wifi_has_ip(NULL, 0)};
    ota_trial_wifi_in(&seen, in);
}

// A minute before the Wi-Fi rollback (#411): a person nearby sees it coming (docs/ui-standard.md rules 5 and 10)
static void trial_warn(void *ctx, uint32_t left_s) {
    (void)ctx;
    ESP_LOGW(TAG, "trial image still without Wi-Fi: rolling back in %u s", (unsigned)left_s);
    lichen_sys_notify(LN_WARN, "Update not confirmed", "No Wi-Fi yet: going back to the previous version in 1 min", 0);
}

static bool trial_mark_valid(void *ctx) {
    (void)ctx;
    return ota_image_mark_valid(); // logs its own failure; the loop tries again a second later
}

static void trial_wait(void *ctx) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// The trial loop has ended, any way (#304): from then on an automatic restart on trial need not wait, it rolls back
// whenever it comes
static atomic_bool trial_settled;

// The rule system_restart asks before an automatic restart (#304; ota_valid.c decides, host_tests/restart_gate_test.c)
static system_restart_gate_t restart_trial_gate(bool asked, bool power_off, bool known_only, char const **note) {
    ota_image_state_t image = known_only ? ota_image_trial_known() : ota_image_trial_state();
    if (known_only && image == OTA_IMAGE_UNKNOWN) {
        return SYSTEM_RESTART_GO; // not read yet, and reading it here is what known_only rules out
    }
    restart_gate_t gate = restart_gate_decide(asked, image, atomic_load(&trial_settled), uptime_ms() / 1000, power_off);
    *note = restart_gate_name(gate);
    return gate == RESTART_GATE_GO ? SYSTEM_RESTART_GO
           : gate == RESTART_GATE_WAIT ? SYSTEM_RESTART_WAIT
                                       : SYSTEM_RESTART_ON_TRIAL;
}

// A freshly installed image runs on trial until it has been up for a while; a reset before that boots the previous one
// again. ota_valid.c decides and runs the loop (host_tests/ota_valid_test.c): the C6 link and, with a display, the
// shell; a boot with the display skipped on purpose (fw noscreen) ends its trial without the shell (#293)
static void ota_trial_wait(ota_display_t display) {
    ota_trial_io_t const io = {.read = trial_read, .mark_valid = trial_mark_valid, .wait = trial_wait,
                               .warn = trial_warn, .ctx = &display};
    if (ota_image_arrival() == OTA_ARRIVAL_OVER_AIR && ota_image_trial_pending()) {
        ESP_LOGW(TAG, "on trial: valid once back on Wi-Fi, else a rollback at %d s of uptime%s",
                 OTA_VALID_NEEDS_WIFI_S, ota_image_arrival_test_no_wifi() ? " (dev test arm: Wi-Fi ignored)" : "");
    }
    ota_trial_end_t end = ota_trial_run(&io);
    atomic_store(&trial_settled, true); // before the rollback below, so a waiting restart goes too
    switch (end) {
    case OTA_TRIAL_END_VALID:
        ESP_LOGI(TAG, "this image is now valid%s%s",
                 display == OTA_DISPLAY_SKIPPED ? " (display skipped: the OS is up)" : "",
                 ota_image_arrival() != OTA_ARRIVAL_OVER_AIR ? " (chosen at the console)"
                 : ota_os_wifi_switch_on()                   ? " (back on Wi-Fi)"
                                                             : " (Wi-Fi switched off: not needed)");
        update_screen_image_valid();
        break;
    case OTA_TRIAL_END_ROLL_BACK:
        // No C6 means no Wi-Fi, so nobody can replace this image: let the bootloader roll back
        ESP_LOGE(TAG, "C6 link never came up on a trial image: restarting to roll back");
        system_restart_asked("trial image: no C6 link"); // the rollback is the point
        break;
    case OTA_TRIAL_END_NO_WIFI:
        // Installed over Wi-Fi and not back on it: unattended, nobody could replace it but over USB (#411)
        ESP_LOGE(TAG, "trial image did not rejoin Wi-Fi within %d s: restarting to roll back", OTA_VALID_NEEDS_WIFI_S);
        system_restart_asked(OTA_NO_WIFI_WHY); // the rollback is the point; the old image's banner reads the reason
        break;
    case OTA_TRIAL_END_STAY:
        ESP_LOGW(TAG, "staying on trial (%s): a reset goes back to the previous image",
                 display == OTA_DISPLAY_FAILED ? "the display did not start" : "UI down or a PSRAM trial");
        break;
    case OTA_TRIAL_END_MARK_FAILED:
        ESP_LOGE(TAG, "staying on trial: marking this image valid failed %d times, a reset goes back to the previous "
                      "image",
                 OTA_TRIAL_TRIES);
        break;
    case OTA_TRIAL_END_NO_STATE:
        ESP_LOGE(TAG, "the OTA state could not be read %d times: if this image is on trial, a reset rolls it back",
                 OTA_TRIAL_TRIES);
        break;
    case OTA_TRIAL_END_NOT_ON_TRIAL:
    default:
        break;
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
    system_restart_set_trial_gate(restart_trial_gate); // before any boot step can restart (#304)
    boot_ctx_t ctx = {0};
    // boot_order.c is the order (the two-chip contract, host_tests/boot_order_test.c); this only walks it
    for (size_t i = 0; i < boot_order_len; i++) {
        start_fns[boot_order[i]](&ctx);
    }
    atomic_store(&boot_steps_done, true);
    ota_trial_wait(ctx.display_is);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
