#pragma once

#include "restart_log.h"

// Serial console on UART0 (#61): lines starting with "fw " are commands, everything else is ignored.
// Replies are printed as "fw: ...". Modules add their own commands with console_register().

#include <stdbool.h>
#include <stddef.h>

#define FW_NVS_NAMESPACE "badge_fw"

typedef void (*console_fn_t)(char const *args); // args: the text after the command name, "" if none

void console_start(void);
void console_register(char const *name, char const *usage, console_fn_t fn);
void console_reply(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
// Like console_reply for a line that holds a secret (a channel key, the update token, a repeater's guest password; #123):
// it reaches only the person who asked: the UART when typed there (a command run through console_exec prints only
// "fw: (secret line)" on the UART), or the Wi-Fi console's reply (console_exec_with_secrets). It is never given to the tap (the log store), the Console app shows a note instead (console_exec), and the formatted copy is
// wiped. A caller wipes its own copies of the secret after the call.
void console_reply_secret(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

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
// True when the calling task runs a line typed on the UART, false inside console_exec (Wi-Fi POST /console). For
// commands only a person at the cable may run (#116).
bool console_from_uart(void);
void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx);
// The same, but secret lines are collected too: for POST /console, whose reply goes only to the one who asked (with the
// update token). The caller wipes buf after done.
void console_exec_with_secrets(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx);

// Every deliberate restart goes through here (console, OTA, slot switch, C6 reflash, after an update), not through a
// bare esp_restart(). The screen is blanked by a shutdown handler (backlight.c), so any esp_restart() gets it.
// why: a short reason, kept with the fw version, uptime and time in NVS (last 5, "fw crash") so an unexplained restart
// can be traced (#41). A bare esp_restart() is caught too and recorded as "unknown caller" with its return address.
void system_restart(char const *why) __attribute__((noreturn));
// The OS's saves before every system_restart (not a bare esp_restart, not a panic): the apps stop as for an update, so
// MeshCore saves its files and flushes its message journal (#123). They run on a task of their own; system_restart
// waits for them up to SYSTEM_RESTART_SAVE_MS, then restarts anyway (logged). Set once from main.c.
#define SYSTEM_RESTART_SAVE_MS 8000 // a ceiling: MeshCore 1-2 s + the journal up to 3 s + notifications up to 2 s
void system_restart_set_save(void (*save)(void));
// Runs once, on the restarting task, right before the restart (every esp_restart, not panics): the shell draws its
// "Restarting" frame so the lit panel does not show a stale or torn screen (#39). Must return quickly.
void system_restart_set_prepare(void (*prepare)(void));

// Adds a line to the same history without restarting: something worth knowing about this boot (#40: a bad PSRAM tuning)
void system_restart_note(char const *why);

// Where the restart history is kept (the settings service, #235): fw_console opens no NVS. Bound in start_nvs right after
// nvs_boot(); a restart before that is not recorded.
void system_restart_bind_store(fw_get_blob_fn get, fw_set_blob_fn set);
// The recorded restarts, newest first; returns how many
int system_restart_history(restart_log_t *out);
