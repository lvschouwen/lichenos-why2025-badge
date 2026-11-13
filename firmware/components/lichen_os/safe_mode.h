#pragma once
// Crash attribution and safe mode (#232), lichen_os's entry points for main and the OS's own services: not part of
// the gate. The panic handler (linker --wrap of esp_panic_handler, CMakeLists.txt) notes the owner app of the faulting
// task in LP RAM; the next boot turns that into a window entry (safe_mode_policy.h): 2 crashes of one app in 3 boots
// switch its background part off until "fw app background <id> on". An OS or IDF task (the console's, so "fw crash
// test panic"), an ISR, a brownout or a lockup blames nobody.

#include <stdbool.h>
#include <stddef.h>

#define SAFE_MODE_APP_ID_CAP 14 // safe_mode_crashed_app's id buffer (an app id with the NUL)

#ifdef __cplusplus
extern "C" {
#endif

// main.c start_early, once, right after crash_check_early (crashed = crash_recovered()): the window, the saved list of
// apps whose background is off (a settings write only when an app is switched off now), and the "fw app background"
// and "crash" subcommands (bound before lichen_sys_app_console_register). Before the display starts, so the one
// possible flash write stalls nothing on the glass (#31).
void safe_mode_boot_step(bool crashed);
// The app's background part stays off this boot (start_apps skips its start_background). Any task.
bool safe_mode_background_off(char const *id);
// This boot follows a crash attributed to an app: its id into id (cap >= 14) and whether this crash switched its
// background off. False for no crash or nobody to blame. Any task.
bool safe_mode_crashed_app(char *id, size_t cap, bool *switched_off);
// A crash is among the last boots in the window, or this boot follows one (lichen_boot_degraded, #280). False before
// safe_mode_boot_step. Any task.
bool safe_mode_degraded_boot(void);
// psram_speed's hook (#353): the restart that re-arms 120 MHz after a crash is about to happen (true) or did not
// (false). That restart and the boot after it are the same crash's, not a clean boot. Any task, no I/O.
void safe_mode_rearm_mark(bool restarting);

#ifdef __cplusplus
}
#endif
