#pragma once

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

// Appends text (n bytes, no newline) and a '\n'. Output past cap is dropped with CONSOLE_CUT_NOTE (room for it is
// always kept). A secret line is collected only when keep_secret; otherwise the note stands in for it.
void console_capture_add(console_capture_t *c, char const *text, size_t n, bool secret);
