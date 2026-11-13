// One console_exec run's output (#38, #123): see console_capture.h
#include "console_capture.h"

#include <stdio.h>
#include <string.h>

static void add(console_capture_t *c, char const *text, size_t n) {
    if (c->cut || c->cap < sizeof(CONSOLE_CUT_NOTE) + 2) {
        return;
    }
    size_t room = c->cap - c->len - sizeof(CONSOLE_CUT_NOTE); // always keeps room for the note
    if (n + 1 > room) {
        memcpy(c->buf + c->len, CONSOLE_CUT_NOTE, sizeof(CONSOLE_CUT_NOTE) - 1);
        c->len += sizeof(CONSOLE_CUT_NOTE) - 1;
        c->cut  = true;
        return;
    }
    memcpy(c->buf + c->len, text, n);
    c->buf[c->len + n] = '\n';
    c->len            += n + 1;
}

void console_capture_add(console_capture_t *c, char const *text, size_t n, bool secret) {
    if (!secret || c->keep_secret) {
        add(c, text, n);
    } else if (!c->secret_noted) {
        c->secret_noted = true;
        add(c, CONSOLE_SECRET_NOTE, sizeof(CONSOLE_SECRET_NOTE) - 1);
    }
}

console_route_t console_route(bool secret, bool capturing) {
    return (console_route_t){
        .uart    = !secret || !capturing,
        .tap     = !secret,
        .capture = capturing,
    };
}

console_route_t console_route_private(bool capturing) {
    return (console_route_t){.uart = true, .tap = false, .capture = capturing};
}

size_t console_line_fmt(char *out, size_t cap, char const *fmt, va_list ap, size_t *text_len) {
    memcpy(out, CONSOLE_LINE_PREFIX, CONSOLE_LINE_PREFIX_LEN);
    size_t room = cap - CONSOLE_LINE_PREFIX_LEN - 2; // the newline and the NUL
    int    n    = vsnprintf(out + CONSOLE_LINE_PREFIX_LEN, room + 1, fmt, ap);
    size_t len  = n < 0 ? 0 : ((size_t)n < room ? (size_t)n : room);
    out[CONSOLE_LINE_PREFIX_LEN + len]     = '\n';
    out[CONSOLE_LINE_PREFIX_LEN + len + 1] = '\0';
    if (text_len) {
        *text_len = len;
    }
    return CONSOLE_LINE_PREFIX_LEN + len + 1;
}
