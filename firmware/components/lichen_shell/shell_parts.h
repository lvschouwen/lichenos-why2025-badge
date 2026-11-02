#pragma once

#include "shell.h"
#include "thermal_rules.h"

// Between the shell's own files (#66), not for apps

extern shell_sources_t shell_sources;

lv_obj_t *boot_screen_create(void);       // the boot screen; calls shell_home() itself when boot is done
lv_group_t *boot_screen_group(void);     // the keypad group of the boot screen, NULL once it is gone
lv_obj_t *status_bar_create(lv_obj_t *parent); // on the top layer, over every screen after boot

// home.c
lv_obj_t *home_create(void);   // a fresh home screen for the registered apps and the saved layout; focus object = itself
char const *home_selected_name(lv_obj_t *home); // the selected tile's app name; NULL if none or not a home screen
void      home_init(void);     // registers the "fw home" console command

// shell.c
void shell_esc(void);                // Esc pressed: the top screen's shell_on_esc() hook first, else shell_back()
void shell_back(void);               // like Esc: pops the top screen (later, outside key handling)
lv_group_t *shell_top_group(void);   // the keypad group of the screen on top (ui_probe, LVGL context)
void shell_open_app(size_t index);     // opens lichen_app_at(index); from home's Enter
void shell_rebuild_home(void);         // after the app list or the layout changed
void shell_rebuild_home_below(void);   // the same, keeping the screens above home open

// ui_probe.c: bench diagnostics
void ui_probe_init(void);           // registers "fw ui"
void ui_probe_send(char const *seq); // "fw keys send <seq>"
void ui_check_start(void);           // #89: 1 s LVGL object-tree check, registers "fw uicheck"
// ui_perf.c: frame timing from the display events (#93); "fw ui perf ..." (ui_probe.c dispatches it)
void ui_perf_start(void);
void ui_perf_cmd(char const *args);

// app_settings.c, app_about.c: the built-in apps (shell_start registers them)
void settings_app_register(void);
void about_app_register(void);
void files_app_register(void); // app_files.c (#71): browse /storage and the SD card
void keytest_app_register(void);
void clock_app_register(void); // app_clock.c (#122)
void bench_app_register(void); // app_bench.c (#96); also registers "fw bench"
bool bench_running(void);      // app_bench.c: a run is measuring (LVGL task)
void power_app_register(void);                   // app_power.c (#65): Restart / Power off, also "fw poweroff"
void power_open_from_settings(void *arg);        // Settings > Power: opens the Power app
void power_request_restart(void);                // Power > Restart without the question (factory reset, #66)
// factory_screen.c: Settings > Factory reset (#66; menu callback, LVGL context)
void factory_screen_open(void *arg);
// update_screen.c: every app's stop_for_update (a firmware update, a restart or power off from the Power app, #65);
// true when all of them stopped. Blocks for seconds: never on the LVGL task.
bool shell_stop_apps(void);
void crashlog_app_register(void); // app_crashlog.c (#72); after a crash also asks on home to open it

// time_set_screen.c: Settings > Time > Set date & time (#64), a builder for shell_open()
lv_obj_t *time_set_build(void);

// wifi_screen.c: Settings > Wi-Fi
void wifi_screen_open(void);
// The same screen as the first-run Wi-Fi step: a Skip row on top; done() runs (LVGL task) after a join or a skip
void wifi_screen_open_setup(void (*done)(void));

// sd_screen.c: Settings > SD card (#99)
void sd_screen_open(void);

// setup_screens.c: first-run setup
void setup_init(void);  // registers "fw setup reset"
void setup_start(void); // opens the welcome on top of home unless setup_done is saved
bool setup_running(void); // the first-run setup is on screen (LVGL task)

// screen_idle.c: the screen timeout (#43)
void screen_idle_init(void);  // registers "fw idle"; from shell_start
void screen_idle_start(void); // once the keyboard is attached: the key hook and the timer (LVGL lock held)
// A key press reached the badge (the keyboard task, or "fw keys send" on the LVGL task): restarts the idle clock and
// lights the screen. True when the key only woke a dark screen and must not act.
bool screen_idle_key(void);
// Settings > Display changed the times, or the screensaver / lock switches (#75)
void screen_idle_reload(void);
// "fw screensaver test" (#75): the saver now, as if the dim time had come, until the next key
void screen_idle_force_saver(void);
// Shown with the stage the screen is in: "bright", "dim", "screensaver", "off"
char const *screen_idle_stage_name(void);

// shell.c (#75): screen (NULL: none) is shown in front of the screen stack until it is taken away; status_bar_shown
// false hides the status bar meanwhile. Only once home is shown (shell_home_shown). LVGL context.
void shell_set_cover(lv_obj_t *screen, bool status_bar_shown);

// screen_saver.c (#75): the clock screensaver and the pocket lock, both covers. LVGL context unless noted.
void screen_saver_init(void);          // registers "fw screensaver" and "fw lock"; from shell_start
void screen_saver_show(bool on);       // screen_idle's tick: the idle stage wants the saver (or not)
void screen_saver_reload(void);        // the saver's style setting changed (#119); any task, from screen_idle_reload
void screen_lock_engage(void);         // the lock screen until the combo; nothing when already locked
void screen_lock_release(void);        // unlocks (the combo, an update, power off, a warning that must show)
void screen_lock_probe_key(uint32_t lv_key); // "fw keys send" while locked: only Esc/Backspace reach the combo (#112)
void screen_lock_probe_end(void);      // the sequence is over: let go of the probe's keys
bool screen_lock_active(void);         // any task
void screen_lock_wake(void);           // any task: unlock and light the screen for something that must be seen

// dialog.c: the open dialog's keypad group, NULL when none is open (#14 T-B); shell.c: give the keys to the dialog, or
// back to the top screen once it is gone
lv_group_t *shell_dialog_group(void);
void        shell_refocus(void);

// shell.c: home is on screen and its fade-in is over (screens may open)
bool shell_home_shown(void);

void update_screen_init(void); // update_screen.c; from shell_start, with the LVGL lock held

// The shape keys as OS shortcuts (#79). shortcuts.c: the raw key listener (shell_start, LVGL lock held), the key itself
// (also "fw keys send square ..."), and the app history ✕ switches in (shell_open_app tells it). LVGL context.
void shortcuts_start(void);
void shortcuts_key(uint8_t code, bool pressed, bool test_open);
void shortcuts_app_opened(size_t index);
// shell.c: ● runs the top screen's shell_on_search() hook; false when it has none
bool shell_search(void);
// app_power.c: Restart or Power off is under way (the goodbye screen is up); any task
bool power_going_down(void);

// Notifications (#62). notify_ui.c: the toast, the tile badges, the cloud key; from shell_start, LVGL lock held.
#define NOTIFY_APP_ID "notify"
void     notify_ui_start(void);
uint32_t notify_level_color(uint8_t level); // dim / amber / red, the toast's stripe and the status bar's dot
// notify_centre.c: the Notifications app (registered by shell_start), opening one notice where it belongs, the centre,
// Settings › Notifications. LVGL context.
void notify_app_register(void);
void notify_open_entry(uint32_t id);
void notify_open_centre(void);
void notify_settings_open(void);
void     notify_ui_cloud_key(void); // ☁ (#79, shortcuts.c): open the showing toast, else the centre
// sys_notices.c: Lichen's own notifications (crash, PSRAM, clock, update, C6, SD, sensors, storage); from shell_start
void sys_notices_start(void);

// The thermal watchdog (#10). thermal.c: the service on core 0 (from shell_start), `fw thermal`; app_battery.c: Settings
// > Battery and the full-screen warning; app_power.c: power_request_off() is Power > Power off without the question.
typedef struct {
    th_level_t level;
    bool       blind;          // no source has answered for TH_BLIND_S
    bool       simulated;      // `fw thermal test` is feeding the board source
    bool       valid[TH_SRC_COUNT];
    float      c[TH_SRC_COUNT]; // what the rules see (the board value has the heater bias taken off)
    float      bme_raw_c;
    uint32_t   implausible[TH_SRC_COUNT];
    th_src_t   cause;
    bool       cause_rise;
    float      cause_c;
} thermal_info_t;
void thermal_start(void);
void thermal_info(thermal_info_t *out);
void battery_open(void *arg);        // Settings > Battery (menu callback); LVGL context
void battery_open_alert(void);       // the same page headed by the warning; any task
void power_request_off(void);        // any task; the goodbye screen, saves, then the #65 power-off path
