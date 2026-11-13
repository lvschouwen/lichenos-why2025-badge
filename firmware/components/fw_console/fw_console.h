#pragma once

// C++ includes it too (MeshCore's mesh_task.cpp)
#ifdef __cplusplus
extern "C" {
#endif

#include "dev_mode.h"
#include "restart_log.h"

// LICHEN_DEV_COMMANDS decides console_register_dev below; host tests have no sdkconfig.h (a release-like build)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

// Serial console on UART0 (#61): lines starting with "fw " are commands, everything else is ignored.
// Replies are printed as "fw: ...". Modules add their own commands with console_register().

#include <stdbool.h>
#include <stddef.h>

#define FW_NVS_NAMESPACE "badge_fw"

typedef void (*console_fn_t)(char const *args); // args: the text after the command name, "" if none

void console_start(void);
void console_register(char const *name, char const *usage, console_fn_t fn);

// Developer mode (#373, dev_mode.h). A debug or test command registers with console_register_dev: while developer mode
// is off it answers DEV_MODE_OFF_NOTE and "fw help" leaves it out. With LICHEN_DEV_COMMANDS off (the release build) the
// call is an empty inline, so the command, its usage and its handler are not in the image.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
void console_register_dev(char const *name, char const *usage, console_fn_t fn);
#else
static inline void console_register_dev(char const *name, char const *usage, console_fn_t fn) {
    (void)name;
    (void)usage;
    (void)fn;
}
#endif
// For a command where only some subcommands are debug ones: true while developer mode is on; otherwise it replies
// "<what>: " DEV_MODE_OFF_NOTE and is false. what names the subcommand, e.g. "ui lockcheck".
bool             console_dev_gate(char const *what);
// Developer mode now (always false in a release build). Any task.
bool             console_dev_mode(void);
// Turns developer mode on or off and saves it (dev.mode). For the owner's own path, About's taps (#374); `fw dev on`
// adds the USB-only rule. No effect in a release build. False when it could not be saved (it still holds this boot).
bool             console_dev_set(bool on);
// The saved state through the settings service (#235); read once here. Bound in settings_bind_dev_mode(), after the
// factory reset step.
void             console_bind_dev_store(fw_get_u8_fn get, fw_set_u8_fn set);
// Where the command running on the calling task came from: the console_exec run's origin, UART on the console task,
// UNKNOWN on any other task
console_origin_t console_origin(void);
void console_reply(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
// Like console_reply for a line that holds a secret (a channel key, the update token, a repeater's guest password; #123):
// it reaches only the person who asked: the UART when typed there (a command run through console_exec prints only
// "fw: (secret line)" on the UART), or the Wi-Fi console's reply (console_exec_with_secrets). It is never given to the tap (the log store), the Console app shows a note instead (console_exec), and the formatted copy is
// wiped. A caller wipes its own copies of the secret after the call.
void console_reply_secret(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
// Like console_reply for a line that is private but not secret (a file's contents, "fw sd cat", #378): whoever asked
// sees it whole (the UART, the Wi-Fi console's reply, the Console app's screen), but it is never given to the tap, so it
// never reaches the log store (GET /log, the log viewer). The formatted copy is wiped.
void console_reply_private(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

// Gets every console_reply line (without the "fw: " prefix) for the log store (#38). Set once at boot.
void console_set_tap(void (*tap)(char const *text, size_t len));
// Adds lines to "fw mem" after the heaps: lichen_mem.c's per-app use (#181; fw_console cannot depend on lichen_api,
// which uses it). Set once at boot.
void console_set_mem_report(void (*report)(void));
// Called at the start and the end of every command (UART and Wi-Fi): the remote key lock's timeout counts from the
// last one (#190). Must be quick. Set once at boot.
void console_set_activity(void (*activity)(void));

// Runs one command line (with or without the "fw " prefix) on the calling task and collects what console_reply prints
// from it in buf (cap bytes; output past that is cut with a note). A console_reply_secret line becomes one
// CONSOLE_SECRET_NOTE (console_capture.h): this is for output kept on the screen (the Console app). done runs exactly
// once with the output: when the command returns, or earlier if it restarts the badge (system_restart calls it first,
// so a reply can be sent before the restart). Output of printf/ESP_LOG calls is not collected. Not reentrant. The
// caller wipes buf after done.
typedef void (*console_done_fn)(void *ctx, char const *out, size_t len);
// True when the calling task runs a line typed on the UART, false inside console_exec (Wi-Fi POST /console, the Console
// app) and on any other task. For commands only a person at the cable may run (#116); console_origin() tells the other
// two apart.
bool console_from_uart(void);
void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx);
// The same, but secret lines are collected too: for POST /console, whose reply goes only to the one who asked (with the
// update token). The caller wipes buf after done.
void console_exec_with_secrets(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx);

// Every deliberate restart goes through here or system_restart_asked (console, OTA, slot switch, C6 reflash, after an
// update), not through a bare esp_restart(). The screen is blanked by a shutdown handler (backlight.c), so any
// esp_restart() gets it.
// why: a short reason, kept with the fw version, uptime and time in NVS (last 5, "fw crash") so an unexplained restart
// can be traced (#41). A bare esp_restart() is caught too and recorded as "unknown caller" with its return address.
// system_restart is a restart the badge makes on its own (a failed update, the C6 supervisor, MeshCore's remote
// reboot): while the running image is on trial it first waits for the trial gate (#304), polled once a second on the
// calling task, so a new caller is safe by default. Never from the LVGL task or with a lock others need while it may
// wait: ask system_restart_would_wait() first and try again later.
void system_restart(char const *why) __attribute__((noreturn));
// A restart a person asked for (fw reboot, /reboot, the Power menu, a factory reset) or one meant to roll the image back
// (the trial's own rollback, a good install's reboot): never waits. On trial it goes back to the previous version; the
// caller says so (system_restart_on_trial).
void system_restart_asked(char const *why) __attribute__((noreturn));

// What a restart does now while the image may be on trial (#304). main binds the rule (ota_valid.c
// restart_gate_decide): fw_console knows nothing of OTA. note: set with SYSTEM_RESTART_ON_TRIAL, why it goes ahead.
// known_only: answer from what is already known, never read the OTA data (a bare esp_restart may come from a fault path
// with the flash cache off): a state not read yet is GO.
typedef enum {
    SYSTEM_RESTART_GO,       // not on trial
    SYSTEM_RESTART_WAIT,     // an automatic restart on trial: look again in a second
    SYSTEM_RESTART_ON_TRIAL, // goes ahead on trial: the bootloader boots the previous version
} system_restart_gate_t;
typedef system_restart_gate_t (*system_restart_gate_fn)(bool asked, bool power_off, bool known_only, char const **note);
// Set once from main.c, early in the boot; NULL (or before it is bound) every restart goes ahead. Any task.
void system_restart_set_trial_gate(system_restart_gate_fn gate);
// An automatic restart now would wait for the gate: for callers that must not block (the update screen's LVGL tick,
// the mesh task). Any task.
bool system_restart_would_wait(void);
// A restart now goes back to the previous version (the image may be on trial): a person's reboot says so. Any task.
bool system_restart_on_trial(void);
// The words a person-asked restart on trial adds to its reply
#define SYSTEM_RESTART_TRIAL_NOTE "this version is still on trial: restarting goes back to the previous one"
// The OS's saves before every system_restart (not a bare esp_restart, not a panic): the apps stop as for an update, so
// MeshCore saves its files and flushes its message journal (#123). They run on a task of their own; system_restart
// waits for them up to SYSTEM_RESTART_SAVE_MS, then restarts anyway (logged). Set once from main.c.
#define SYSTEM_RESTART_SAVE_MS 8000 // a ceiling: MeshCore 1-2 s + the journal up to 3 s + notifications up to 2 s
void system_restart_set_save(void (*save)(void));
// The same saves with the same bound, without the restart: a power off that has no Power app to run them (the thermal
// shutdown on a badge whose display did not come up, #281). Once per boot: a restart after it saves nothing more.
void system_save_before_off(void);
// A power off was asked (#281: the thermal shutdown, `fw poweroff`). From now on every restart, system_restart or a
// bare esp_restart (fw reboot, an update, a slot switch), calls off after its saves instead of restarting, so a badge
// that must go off for heat never reboots instead. off must not return. Any task.
void system_power_off_requested(void (*off)(void));
// Runs once, on the restarting task, right before the restart (every esp_restart, not panics): the shell draws its
// "Restarting" frame so the lit panel does not show a stale or torn screen (#39). Must return quickly.
void system_restart_set_prepare(void (*prepare)(void));

// Adds a line to the same history without restarting: something worth knowing about this boot (#40: a bad PSRAM tuning)
void system_restart_note(char const *why);
// Names the next bare esp_restart() in the restart history instead of "unknown caller" (the caller address stays). why
// must be a string literal (kept by pointer). For an early-boot restart that is deliberate but cannot go through
// system_restart (no save hooks yet).
void system_restart_name_next(char const *why);

// Where the restart history is kept (the settings service, #235): fw_console opens no NVS. Bound in start_nvs right after
// nvs_boot(); a restart before that is not recorded.
void system_restart_bind_store(fw_get_blob_fn get, fw_set_blob_fn set);
// The recorded restarts, newest first; returns how many
int system_restart_history(restart_log_t *out);

#ifdef __cplusplus
}
#endif
