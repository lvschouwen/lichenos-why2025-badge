#pragma once

#include "restart_log.h"

// Serial console on UART0 (#61): lines starting with "fw " are commands, everything else is ignored.
// Replies are printed as "fw: ...". Modules add their own commands with console_register().

#include <stddef.h>

#define FW_NVS_NAMESPACE "badge_fw"

typedef void (*console_fn_t)(char const *args); // args: the text after the command name, "" if none

void console_start(void);
void console_register(char const *name, char const *usage, console_fn_t fn);
void console_reply(char const *fmt, ...) __attribute__((format(printf, 1, 2)));
// Like console_reply for secrets (the update token): UART only, never stored in the log (#38) or sent over Wi-Fi
void console_reply_private(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

// Gets every console_reply line (without the "fw: " prefix) for the log store (#38). Set once at boot.
void console_set_tap(void (*tap)(char const *text, size_t len));

// Runs one command line (with or without the "fw " prefix) on the calling task and collects what console_reply prints
// from it in buf (cap bytes; output past that is cut with a note). done runs exactly once with the output: when the
// command returns, or earlier if it restarts the badge (system_restart calls it first, so a reply can be sent before
// the restart). Output of printf/ESP_LOG calls is not collected. Not reentrant.
typedef void (*console_done_fn)(void *ctx, char const *out, size_t len);
void console_exec(char const *line, char *buf, size_t cap, console_done_fn done, void *ctx);

// Every deliberate restart goes through here (console, OTA, slot switch, C6 reflash, after an update), not through a
// bare esp_restart(). The screen is blanked by a shutdown handler (backlight.c), so any esp_restart() gets it.
// why: a short reason, kept with the fw version, uptime and time in NVS (last 5, "fw crash") so an unexplained restart
// can be traced (#41). A bare esp_restart() is caught too and recorded as "unknown caller" with its return address.
void system_restart(char const *why) __attribute__((noreturn));

// The recorded restarts, newest first; returns how many
int system_restart_history(restart_log_t *out);
