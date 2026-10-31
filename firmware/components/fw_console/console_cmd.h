#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Picks "badgevms <command>" lines out of the console input stream; every other byte goes on to app stdin.
// Pure logic, no ESP headers: built into the host tests as well as the firmware.

// The lean firmware (#61) builds this splitter with its own prefix
#ifndef CONSOLE_CMD_PREFIX
#define CONSOLE_CMD_PREFIX "badgevms "
#endif
#define CONSOLE_CMD_MAX    96

typedef struct {
    char   buf[CONSOLE_CMD_MAX]; // held prefix bytes, or the command after the prefix
    size_t len;
    bool   at_line_start;
    bool   in_command;
    bool   overflow;
    bool   swallow_newline; // the CR or LF that follows a command's own line end
} console_cmd_t;

typedef enum {
    CONSOLE_CMD_IDLE,     // nothing to do yet
    CONSOLE_CMD_PASS,     // *pass/*pass_len are bytes for stdin
    CONSOLE_CMD_READY,    // s->buf holds a NUL-terminated command (without the prefix)
    CONSOLE_CMD_TOO_LONG, // a command line was longer than CONSOLE_CMD_MAX - 1 and was dropped
} console_cmd_result_t;

void console_cmd_init(console_cmd_t *s);

// Feeds one byte. The prefix only counts at the start of a line; bytes that start to match it are held back until
// the match succeeds or fails, so stdin sees them a little later but never loses them.
console_cmd_result_t console_cmd_feed(console_cmd_t *s, char c, char const **pass, size_t *pass_len);

// Call when the input has been idle for a moment: held prefix bytes go to stdin after all (a lone 'b' typed at a
// prompt must not wait for the next key), and a pending line-end swallow is cancelled. Never interrupts a command
// that is being typed.
console_cmd_result_t console_cmd_flush(console_cmd_t *s, char const **pass, size_t *pass_len);

// A C6 image identifier as used in APPS:[<id>]: 1-48 characters of a-z, 0-9 and '_'
#define CONSOLE_C6_ID_MAX 48
bool console_c6_id_valid(char const *id);

// The duration argument of "c6log [ms]": empty means CONSOLE_C6LOG_DEFAULT_MS, otherwise 1-CONSOLE_C6LOG_MAX_MS digits
#define CONSOLE_C6LOG_DEFAULT_MS 10000
#define CONSOLE_C6LOG_MAX_MS     60000
bool console_c6log_ms(char const *arg, uint32_t *ms);

// The arguments of "memhog <bytes-left> <seconds>": bytes-left 0-CONSOLE_MEMHOG_MAX_LEFT, seconds 1-CONSOLE_MEMHOG_MAX_S,
// separated by one space
#define CONSOLE_MEMHOG_MAX_LEFT (16u * 1024 * 1024)
#define CONSOLE_MEMHOG_MAX_S    600
bool console_memhog_args(char const *arg, uint32_t *left, uint32_t *secs);
