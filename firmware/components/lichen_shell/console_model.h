#pragma once

#include <stdbool.h>
#include <stddef.h>

// The pure part of the Console app (#150): the history of typed lines, the secret-line filter and the capped output
// buffer. Pure so it is host-tested (host_tests/console_model_test.c); app_console.c holds the screen and the worker.

#define CONSOLE_LINE_MAX 160 // the longest line the entry takes (a UART line is shorter, a Wi-Fi one longer)
#define CONSOLE_HIST_N   16

// A line that may hold a password or a login (an extra deny on top of the allow-list): never kept in the history and not echoed into the output. Matches
// "password" and "login" anywhere, in any case ("wifi password ...", "mesh remote login ...").
bool console_line_is_secret(char const *line);

// A line that is safe to keep and to show: its command (and subcommand where it matters) is on an allow-list of commands
// that never take a secret argument, no word is YES (a confirm is typed each time, never recalled), and it is not
// secret by the keyword check above. Everything else is hidden: not stored, echoed as "> <first word> (arguments
// hidden)". A leading "fw " is ignored.
bool console_line_is_public(char const *line);
// The first word of line (after a leading "fw ") copied to out
void console_first_word(char const *line, char *out, size_t n);

typedef struct {
    char line[CONSOLE_HIST_N][CONSOLE_LINE_MAX + 1]; // a ring: [start] is the oldest
    int  start;
    int  count;
    int  pos; // browsing: 0 is the newest, count - 1 the oldest; -1 when not browsing
} console_hist_t;

void console_hist_init(console_hist_t *h);
// Stores a line; false when it is not kept (empty, secret, or the same as the newest). Ends browsing.
bool console_hist_push(console_hist_t *h, char const *line);
// Up: the next older line; NULL when there is none (empty history) so the caller leaves the entry as it is. At the
// oldest line it stays there and returns it again.
char const *console_hist_older(console_hist_t *h);
// Down: the next newer line; "" past the newest (browsing ends); NULL when not browsing.
char const *console_hist_newer(console_hist_t *h);
bool        console_hist_browsing(console_hist_t const *h);
void        console_hist_end_browse(console_hist_t *h);

// Appends text to the NUL-terminated output buffer (cap bytes, len used) and returns the new length. When it does not
// fit, the oldest whole lines are dropped; text longer than the buffer keeps its newest part, from a line start.
// Control characters other than '\n' become spaces so the label shows no missing-glyph boxes.
size_t console_out_append(char *buf, size_t cap, size_t len, char const *text, size_t n);
