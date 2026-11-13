#pragma once

#include "shell.h"
#include "lichen_sys_thermal.h"
#include "radio_state.h"
#include "draw_stack.h"

// Between the shell's own files (#66), not for apps

extern shell_sources_t shell_sources;

lv_obj_t *boot_screen_create(void);       // the boot screen; calls shell_home() itself when boot is done
lv_group_t *boot_screen_group(void);     // the keypad group of the boot screen, NULL once it is gone
lv_obj_t *status_bar_create(lv_obj_t *parent); // on the top layer, over every screen after boot
lv_obj_t *shell_status_bar(void);              // the bar once created (NULL before); `fw ui overflow` keeps content off it
// "Keys locked: hold Esc 3 s" over the clock and the right group (on the top layer, so also over covers that hide the
// bar) while "fw ui lock" holds (#190); HOT and the C6 state stay visible. LVGL context, nothing before the bar exists
void status_bar_remote_lock(bool on);

// home.c
lv_obj_t *home_create(void);   // a fresh home screen for the registered apps and the saved layout; focus object = itself
char const *home_selected_name(lv_obj_t *home); // the selected tile's app name; NULL if none or not a home screen
void      home_init(void);     // registers the "fw home" console command
// Arrange mode (#189): on starts it on this home screen, off ends it (a held tile goes back). LVGL context.
void home_arrange(lv_obj_t *home, bool on);
bool home_arrange_esc(void *home); // home's shell_on_esc hook: a held tile goes back, else arranging ends
bool home_order_arranged(void);    // a saved order differs from the registration order (any task)

// shell.c
void shell_esc(void);                // Esc pressed: the top screen's shell_on_esc() hook first, else shell_back()
lv_group_t *shell_top_group(void);   // the keypad group of the screen on top (ui_probe, LVGL context)
void shell_open_app(size_t index);     // opens lichen_app_at(index); from home's Enter
void shell_rebuild_home(void);         // after the app list or the layout changed
void shell_rebuild_home_below(void);   // the same, keeping the screens above home open
void shell_arrange_home(bool rebuild); // Settings > Display > Arrange home: back to home (rebuilt first), arranging

// ui_probe.c: bench diagnostics
void ui_probe_init(void);           // registers "fw ui"
void ui_probe_send(char const *seq); // "fw keys send|press <seq>": through the real key path (#375)
void ui_check_start(void);           // #89: 1 s LVGL object-tree check, registers "fw uicheck"
// card_overlay.c (#352): "fw ui card [test|white|grey|black|off]", the bench test card over the current screen
void card_overlay_init(void);            // sets the fw label the card shows
void card_overlay_cmd(char const *args); // console task: args after "card"
bool test_cards_open(void);              // LVGL task: a test card is up, this overlay or the Test card app (#355)
// draw_call.c (#255, #259): full-screen draws (lv_refr_now) only on a stack with DRAW_STACK_NEED free (draw_stack.h)
bool   shell_note_ui_task(void); // shell_start: queues the note of the LVGL task's handle; false when not queued
bool   shell_on_ui_task(void);
size_t shell_stack_free_here(void);
// Runs fn(arg) under the LVGL lock on a stack big enough for lv_refr_now: here, or on the LVGL task while this one
// waits. Called without the lock. Anything but DRAW_CALL_RAN: fn did not run
typedef enum {
    DRAW_CALL_RAN,
    DRAW_CALL_NO_UI,   // no display (or no memory to queue the hand-over)
    DRAW_CALL_NO_TASK, // the LVGL task is not known yet (before shell_start), or is the caller and short of stack
} draw_call_t;
draw_call_t shell_draw_call(void (*fn)(void *), void *arg);

// ui_perf.c: frame timing from the display events (#93); "fw ui perf ..." (ui_probe.c dispatches it)
void ui_perf_early(lv_display_t *disp); // the display events, from the boot screen on, so the first flush is seen (#205)
void ui_perf_watch_keys(lv_indev_t *keys); // stamps key presses for the key-to-flush latency (#205)
void ui_perf_start(void);
void ui_perf_cmd(char const *args);

// app_settings.c, app_about.c: the built-in apps (shell_start registers them)
void settings_app_register(void);
void about_app_register(void);
void files_app_register(void); // app_files.c (#71): browse /storage and the SD card
void bench_app_register(void); // app_bench.c (#96); also registers "fw bench"
bool bench_running(void);      // app_bench.c: a run is measuring (LVGL task)
// attention.c (#164): the keyboard light pulse for a new message and the flashlight (Settings > Display, "fw flashlight")
void attention_init(void);                           // registers "fw flashlight"; from shell_start
void attention_on_toast(uint32_t id, uint8_t level); // notify_ui.c, LVGL task: a toast with feedback starts to show
void flashlight_open(void);                          // LVGL context
bool flashlight_active(void);                        // any task: the screen timeout keeps the screen bright meanwhile
void power_app_register(void);                   // app_power.c (#65): Restart / Power off, also "fw poweroff"
void power_open_from_settings(void *arg);        // Settings > Power: opens the Power app
void power_request_restart(void);                // Power > Restart without the question (factory reset, #66)
// factory_screen.c: Settings > Factory reset (#66; menu callback, LVGL context)
void factory_screen_open(void *arg);
void backup_screen_open(void *arg); // backup_screen.c: Settings > Backup (#169)
// app_console.c: Settings > For developers > Console (#150; menu callback, LVGL context)
void console_open_from_settings(void *arg);
// update_screen.c: every app's stop_for_update (a firmware update, a restart or power off from the Power app, #65);
// true when all of them stopped. Blocks for seconds: never on the LVGL task.
bool shell_stop_apps(void);
void crashlog_app_register(void); // app_crashlog.c (#72); after a crash also asks on home to open it

// time_set_screen.c: Settings > Time > Set date & time (#64), a builder for shell_open()
lv_obj_t *time_set_build(void);

// wifi_screen.c: Settings > Wi-Fi
void wifi_screen_open(void);
// wifi_analyser_screen.c: Settings > Wi-Fi > Wi-Fi analyser (#159)
void wifi_analyser_open(void);
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
// A key press reached the badge (the keyboard task's wake hook, "fw keys send" included, #375): restarts the idle clock and
// lights the screen. True when the key only woke a dark screen and must not act.
bool screen_idle_key(void);
// Settings > Display changed the times, or the screensaver / lock switches (#75)
void screen_idle_reload(void);
// "fw screensaver test" (#75): the saver now, as if the dim time had come, until the next key
void screen_idle_force_saver(void);
// A test card opened or closed (#355): activity, so the idle clock starts again and the screensaver test ends, and the
// pocket lock is lifted for IDLE_CARD_HOLD_S from now. Any task.
void screen_idle_touch(void);
// Fully bright and no screensaver (the keyboard light pulse only runs then, #164)
bool screen_idle_lit(void);
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
bool screen_lock_active(void);         // any task
void screen_lock_wake(void);           // any task: unlock and light the screen for something that must be seen

// dialog.c: the open dialog's keypad group, NULL when none is open (#14 T-B); shell.c: give the keys to the dialog, or
// back to the top screen once it is gone
lv_group_t *shell_dialog_group(void);
void        shell_refocus(void);

// The open dialog as any task sees it (#376; "fw keys send" asks it before a remote Enter, #182). Never takes the LVGL
// lock: it copies the wanted state under dialog.c's own locks, and false (info all zero) when it could not get them in
// time. ms_open and left_s count from when the dialog was built on screen: 0 and the full countdown until then.
#define SHELL_DIALOG_TITLE_MAX 192
#define SHELL_DIALOG_LABEL_MAX 48
typedef struct {
    bool     open;
    bool     careful;
    char     title[SHELL_DIALOG_TITLE_MAX];         // as the dialog layer keeps it (truncated to fit)
    char     confirm_label[SHELL_DIALOG_LABEL_MAX];
    uint32_t ms_open;                              // since it was shown
    uint32_t left_s;                               // of the countdown, rounded up
} shell_dialog_info_t;
bool shell_dialog_info(shell_dialog_info_t *info);

// shell_home_shown (home is on screen and its fade-in is over) is in shell.h: main hands it to the OS watch services

void update_screen_init(void); // update_screen.c; from shell_start, with the LVGL lock held

// The shape keys as OS shortcuts (#79). shortcuts.c: the raw key listener (shell_start, LVGL lock held), the key itself
// (also "fw keys send square ..."), and the app history ✕ switches in (shell_open_app tells it). LVGL context.
void shortcuts_start(void);
void shortcuts_key(uint8_t code, bool pressed, bool test_open);
void shortcuts_app_opened(size_t index);
// shell.c: ● runs the top screen's shell_on_search() hook; false when it has none
bool shell_search(void);
// power_going_down (app_power.c: Restart or Power off is under way) is in shell.h: main's `fw poweroff` reads it

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
// app_settings.c: one registry key's row text from the cache (#332, #333): "<label>: <applied> (<age>)", "(stale N s)",
// "(saved X)" when it is not the saved one, "?" when unread; never waits for the C6. LVGL context not needed.
void shell_reg_text(char const *label, char const *key, char const *saved, char const *unit, char *out, size_t cap);
// The multi-key lines from the cache (#333): LoRa's settings, the Wi-Fi radio's, the PSRAM clock against psram.want
void shell_lora_line(char *out, size_t cap);
void shell_wifi_line(char *out, size_t cap);
void shell_psram_line(char *out, size_t cap);
// A radio switch row's value (#348): the person's switch, "On" or "Off", plus ", held off (hot)" while the thermal
// load shed holds the radio; Enter on the row flips only that switch (lichen_radio_switch_set). Any context.
void shell_radio_value(radio_id_t id, char *out, size_t cap);
void     notify_ui_cloud_key(void); // ☁ (#79, shortcuts.c): open the showing toast, else the centre

// The thermal watchdog (#10) is an OS service since #281 (lichen_os/thermal.c, lichen_sys_thermal.h); app_battery.c is
// Settings > Battery and the full-screen warning (battery_open_alert, shell.h); app_power.c's power_request_off (shell.h)
// is Power > Power off without the question.
void battery_open(void *arg);        // Settings > Battery (menu callback); LVGL context
