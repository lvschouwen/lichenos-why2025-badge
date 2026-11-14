#pragma once
// Crash attribution and safe mode (#232), lichen_os's entry points for main and the OS's own services: not part of
// the gate. The panic handler (linker --wrap of esp_panic_handler, CMakeLists.txt) notes the owner app of the faulting
// task in LP RAM; the next boot turns that into a window entry (safe_mode_policy.h): 2 crashes of one app in 3 boots
// (3 when its screen callbacks crashed, #427) switch its background part off until "fw app background <id> on". An
// OS or IDF task (the console's, so "fw crash test panic"), an ISR, a brownout or a lockup blames nobody.

#include <stdbool.h>
#include <stddef.h>

#define SAFE_MODE_APP_ID_CAP   14 // safe_mode_crashed_app's id buffer (an app id with the NUL)
#define SAFE_MODE_WINDOW_BOOTS 3  // the boots the crashes are counted in (safe_mode_policy.h's window)

#ifdef __cplusplus
extern "C" {
#endif

// main.c start_early, once, right after crash_check_early (crashed = crash_recovered()): the window, the saved list of
// apps whose background is off (read here, written nothing), and the "fw app background" and "crash" subcommands
// (bound before lichen_sys_app_console_register). The list is written later, by safe_mode_background_off in
// start_apps after the shell is up, once and only when it changes (an app switched off now).
void safe_mode_boot_step(bool crashed);
// start_apps, for every registered app: its background part stays off this boot (start_apps skips start_background).
// The app this boot's crash is blamed on is switched off here (and saved) when it reached the limit and may be
// (may_switch_off: it has a background part and is not safe_mode_exempt). The settings are written only when the
// list changes. Main task, at boot.
bool safe_mode_background_off(char const *id, bool may_switch_off);
// Settings > Apps (#233). Whether id is on the list of apps whose background part stays off (a crash put it there, or a
// person did); the switch itself: allowed false adds id to the list, true takes it off and forgets its crashes (the
// same as "fw app background <id> on"). Both apply from the next boot (start_apps reads the list once). True when the
// list holds the wanted state afterwards and it was saved; false (list unchanged on disk, the change kept for this
// boot only) when the settings store refused it or there is no room. Any task but the LVGL task (a settings write).
bool safe_mode_background_listed(char const *id);
bool safe_mode_background_set(char const *id, bool allowed);
// How many of the window's boots ended in a crash named after id, either kind (0 for a bad id). Any task.
unsigned safe_mode_app_crashes(char const *id);

// This boot follows a crash attributed to an app: its id into id (cap >= 14), its crashes in the window (crashes,
// may be NULL: its strikes, or for a callback crash its crashes of both kinds toward the callback limit), whether
// start_apps switched its background off (switched_off, may be NULL) and whether it happened in one of the app's LVGL
// callbacks (in_callback, may be NULL; #349: no strike; #427: 3 in 3 boots switch it off). False for no crash or
// nobody to blame. Any task.
bool safe_mode_crashed_app(char *id, size_t cap, unsigned *crashes, bool *switched_off, bool *in_callback);
// A crash is among the last boots in the window, or this boot follows one (lichen_boot_degraded, #280). False before
// safe_mode_boot_step. Any task.
bool safe_mode_degraded_boot(void);
// psram_speed's hook (#353): the restart that re-arms 120 MHz after a crash is about to happen (true) or did not
// (false). That restart and the boot after it are the same crash's, not a clean boot. Any task, no I/O.
void safe_mode_rearm_mark(bool restarting);

#ifdef __cplusplus
}
#endif
