#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

// The output of one console_exec run (#38), collected line by line. Pure, so it is host-tested
// (host_tests/console_capture_test.c); console.c holds the task and lock around it.
//
// Secret lines (console_reply_secret: a channel key, the update token, a repeater's guest password, #123) go to the one
// who asked and nowhere else. The Wi-Fi console's reply is that person (keep_secret); the Console app's scrollback
// lives on the screen for anyone to read later, so there a secret line becomes CONSOLE_SECRET_NOTE, once per run.

#define CONSOLE_CUT_NOTE    "[output cut]\n"
#define CONSOLE_SECRET_NOTE "(secret output hidden here: run it on the USB or Wi-Fi console to see it)"

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   cut;
    bool   keep_secret;  // the caller is the person who asked (the Wi-Fi reply): secret lines are collected
    bool   secret_noted; // CONSOLE_SECRET_NOTE is in buf already
} console_capture_t;

// Where one console_reply line may go. A secret line (#123) reaches only the one who asked: the UART when the line was
// typed there, the capture when the command runs through console_exec; the log tap never.
typedef struct {
    bool uart;    // printed on the UART (else the UART gets CONSOLE_SECRET_UART, so a bench log shows a line was there)
    bool tap;     // given to the log store
    bool capture; // handed to console_capture_add (which keeps it or puts the note in its place)
} console_route_t;
#define CONSOLE_SECRET_UART "(secret line)"
// capturing: the calling task runs console_exec (the Wi-Fi console or the Console app), not a UART line
console_route_t console_route(bool secret, bool capturing);
// A private line (console_reply_private, #378: a file's contents) goes to whoever asked, whole, and never to the log
// store: the UART when typed there, else only the capture, as an ordinary line (the Wi-Fi reply, the Console app's
// screen; the UART gets CONSOLE_SECRET_UART, as a bench's serial logger keeps it); the tap never
console_route_t console_route_private(bool capturing);

// Appends text (n bytes, no newline) and a '\n'. Output past cap is dropped with CONSOLE_CUT_NOTE (room for it is
// always kept). A secret line is collected only when keep_secret; otherwise the note stands in for it.
void console_capture_add(console_capture_t *c, char const *text, size_t n, bool secret);

// One UART line, "fw: " + text + "\n" + NUL, formatted into out (cap >= 7) so it leaves in ONE write (#327): three
// separate prints let another task's log line land between the prefix and a secret, and the serial logger then logs the
// secret in clear. Text past cap - 6 is cut; the newline is always kept. *text_len is the text's length (it starts at
// out + CONSOLE_LINE_PREFIX_LEN and is followed by the newline). Returns the bytes to write (without the NUL).
#define CONSOLE_LINE_PREFIX     "fw: "
#define CONSOLE_LINE_PREFIX_LEN 4
size_t console_line_fmt(char *out, size_t cap, char const *fmt, va_list ap, size_t *text_len);
