#pragma once

// Serial console on UART0 (#61): lines starting with "fw " are commands, everything else is ignored.
// Replies are printed as "fw: ...". Modules add their own commands with console_register().

#define FW_NVS_NAMESPACE "badge_fw"

typedef void (*console_fn_t)(char const *args); // args: the text after the command name, "" if none

void console_start(void);
void console_register(char const *name, char const *usage, console_fn_t fn);
void console_reply(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

// Every deliberate restart goes through here (console, OTA, slot switch, C6 reflash, after an update), not through a
// bare esp_restart(). The screen is blanked by a shutdown handler (backlight.c), so any esp_restart() gets it.
void system_restart(void) __attribute__((noreturn));
