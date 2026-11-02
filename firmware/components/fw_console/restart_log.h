#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The last restarts and who asked for them (#41). Pure logic, no ESP headers; fw_console keeps one of these in NVS.
#define RESTART_LOG_KEEP 5
#define RESTART_LINE_MAX 112

typedef struct {
    uint32_t count; // entries in use, at most RESTART_LOG_KEEP; line[0] is the newest
    char     line[RESTART_LOG_KEEP][RESTART_LINE_MAX];
} restart_log_t;

// "<when> <version> up=<s>s <why>[ caller=0x%08x]". when NULL: time was not set ("t?"). why is cut to printable ASCII.
// Returns the length written (always NUL terminated).
size_t restart_line_format(char *out, size_t cap, char const *when, char const *version, uint32_t uptime_s, char const *why,
                           uint32_t caller);

// Adds a line as the newest entry; the oldest falls off when full
void restart_log_push(restart_log_t *log, char const *line);

// Makes a log read back from flash safe to print: count clamped, every line NUL terminated
void restart_log_sanitize(restart_log_t *log);

// Before a deliberate restart (#123): what the restarting task does about the apps' saves
typedef enum {
    RESTART_SAVE_NONE, // no save hook (early boot): restart now
    RESTART_SAVE_RUN,  // the first restart asked: run the saves on a task of their own, wait for them up to the bound
    RESTART_SAVE_WAIT, // another restart is saving already and restarts the badge when done: wait, then go anyway
} restart_save_t;
restart_save_t restart_save_plan(bool have_hook, bool already_saving);

#ifdef __cplusplus
}
#endif
