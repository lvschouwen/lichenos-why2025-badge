#pragma once

#include "app_secret_move.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// lichen_os's own entry points for main: not part of the gate, apps never see them.

// Hands the settings to the lower layers that keep a value of their own but may not open NVS (#224): the backlight
// levels and the R16 result (board), the sensors' start-at-boot flag, and (#235) the notification,
// status LED and vibration settings and the restart history. Only stores pointers. Call once, right after
// nvs_boot() and before anything can restart or read a setting (main.c start_nvs).
void settings_bind_services(void);

// The rest of the wiring below the gate (#245, os_bind.c), called by settings_bind_services: lichen_boot's "fw boot
// fail" flag, lichen_clock's source record, the update server's settings and Wi-Fi switch, the factory reset's
// notices, and the notification service's state for the status LED (do-not-disturb, unread) and feedback for the motor.
void os_bind_lower(void);

// A small binary value in the same namespace (the restart history, #235): OS-internal, deliberately not in the gate
// (settings_store.h) so apps cannot read or write raw blobs. get is true only when exactly size bytes are stored.
bool settings_get_blob(char const *key, void *out, size_t size);
bool settings_set_blob(char const *key, void const *data, size_t size);

// Hands the console developer mode's saved state (dev.mode, #373), which it reads once here. Call right after
// factory_reset_boot_nvs(), so a factory reset pending at this boot is already done (main.c start_nvs).
void settings_bind_dev_mode(void);

// The move of an app's plain secrets into its secrets class (#288, app_secret_move.h), in namespace a.<id> for the
// declared prefix (NULL: nothing to do). main.c calls it for every registered app at every boot (a no-op once none are
// left), after factory_reset_boot_nvs() and before anything reads an app's settings. other_migration: another settings migration
// ran this boot (lichen_nvs_migrated()), and two never share a boot: the move then waits for the next one. Logs
// nothing; *count gets the numbers for main.c's one line.
app_secret_move_t app_secrets_move_app(char const *id, char const *prefix, bool other_migration,
                                       app_secret_move_count_t *count);

// Hands lichen_net its store (#235): the SSIDs as plain settings, the Wi-Fi passwords as
// secrets (settings_secret.h). Call once after NVS is up, before wifi_service_init().
void settings_bind_net(void);

// Arms the UI service's debug lock-owner check (#225, CONFIG_LICHEN_UI_LOCK_CHECK_MODE; nothing when off): from here,
// LVGL touched by a task that is neither the LVGL task nor inside lichen_sys_ui_lock is counted (or asserts). Call
// once, after the last boot step that takes the port lock directly (board's display and keyboard setup) and only with
// a display.
void lichen_ui_service_arm(void);

// The LVGL port is up (#248): called once, by the boot step whose display_start succeeded. Until then lichen_sys_ui_lock,
// lichen_sys_ui_call, lichen_ui_call and lichen_ui_with_group refuse (false / nothing) instead of asserting in
// lvgl_port_lock, so a badge with no display still boots to Wi-Fi and OTA.
void lichen_ui_service_port_up(void);

// The settings registry service (#307/#310, lichen_reg.c): the P4 table, the proxies for the C6's keys, `fw get`,
// `fw set` and `fw bl`, and the backlight's REG_SET path. Once, after lora_start (or after it was skipped: the P4's own
// keys work without a C6); it asks c6.reg_caps at once.
void lichen_reg_start(void);
// The display fingerprint line (#354): `fw disp state` and one log line 65 s after boot. Called by lichen_reg_start.
void disp_state_start(void);
// One key's cached applied value as a status line part, never waiting for the C6 (`fw airplane`'s bt.state, #311):
// "idle (12.3 s ago)" or "? (<why>)"; 0 bytes for a name the table does not have
size_t lichen_reg_cached_text(char const *name, char *buf, size_t cap);
// The same cache as a number, for a U8 or U32 key (fw soak, #409: bl.display's applied level): false when the key is
// not one, has no applied value yet, or the C6 stopped answering (stale). Never waits; any task.
bool   lichen_reg_cached_u32(char const *name, uint32_t *out);

// The OS watch services (#281): the thermal watchdog (thermal.c, #10) and Lichen's own notices (sys_notices.c, #62).
// Started by main in their own boot step, display or not, so a badge whose screen did not come up still powers off
// when it gets too hot. What they need from above the OS comes in as hooks; any member may be NULL.
typedef struct {
    // The boot has settled: home is on screen, or without a display main's boot steps are all done (the first-run
    // and boot notices wait for it, so a screenless boot raises no notices about steps still under way). NULL: at once
    bool (*ui_ready)(void);
    // The full-screen heat warning (the shell's Settings > Battery page); any task. NULL: the notice only
    void (*hot_alert)(void);
    // Saves and powers the badge off (the Power app's path, or main's headless one); any task, may return before the
    // badge is down. The watchdog switches the badge off itself 30 s later if it is still up. NULL:
    // lichen_sys_power_off_now (no saves)
    void (*power_off)(void);
    // The update result line of this boot (update_screen_boot_result: 0 none, 1 updated, 2 rolled back)
    int (*update_result)(char *line, size_t cap);
} lichen_watch_hooks_t;

// Copies the hooks, registers `fw thermal` and starts both tasks. Once, after lichen_notify_start.
void lichen_watch_start(lichen_watch_hooks_t const *hooks);
// The boot's own notices (a crash, a PSRAM check, an update, ...) have been raised: a restart from now on keeps them, as
// its saves write the notifications (#353: the re-arm of 120 MHz after a crash waits for this). Any task.
bool lichen_watch_boot_told(void);
// fw soak (#419), since boot, any task, no lock: the /storage count's last and longest wall time (known false before
// the first count), and the notice steps over their slow limit (the "took ... ms" warnings): how many, the longest ms
typedef struct {
    bool     storage_known;
    uint32_t storage_last_ms, storage_max_ms, steps_slow, steps_slow_max_ms;
} lichen_watch_slow_t;
void lichen_watch_slow_counts(lichen_watch_slow_t *out);
void thermal_start(lichen_watch_hooks_t const *hooks);     // lichen_watch_start's thermal half
void sys_notices_start(lichen_watch_hooks_t const *hooks); // and its notices half

// stop(reason)'s report (#231, app_stop.c): ctx's booked tasks still running, other than the caller's, counted now
// without a wait or a log line. Any task, not an ISR (lichen_task.c's spinlock).
size_t lichen_os_task_count(uint32_t ctx); // a lichen_ctx_t (this header stays free of LVGL for the host tests)
