#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Picks "<prefix><command>" lines out of the console input stream; every other byte goes on to app stdin. The badge
// builds it with "fw " (CMakeLists.txt); "lichen " is only the host tests' default.
// Pure logic, no ESP headers: built into the host tests as well as the firmware.

// The lean firmware (#61) builds this splitter with its own prefix
#ifndef CONSOLE_CMD_PREFIX
#define CONSOLE_CMD_PREFIX "lichen "
#endif
// A UART line; the splitter's buffer is part of the console task's 4 KB stack, so it stays small
#define CONSOLE_CMD_MAX    96
// A Wi-Fi console line (POST /console, #110): room for "fw contacts import " and a whole contact link (up to 383
// characters, mesh_share.h), so a link goes in one line there instead of parts. Its buffers are on the heap.
#define CONSOLE_WIFI_CMD_MAX 512

// The longest line the console takes from anywhere (the Wi-Fi console's, UART lines are shorter), bytes with the NUL
size_t console_line_max(void);

// A POST /console body length the Wi-Fi console takes: 1 to CONSOLE_WIFI_CMD_MAX - 1 bytes (negative: none given)
bool console_wifi_len_ok(long content_length);

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
