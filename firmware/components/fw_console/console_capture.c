// One console_exec run's output (#38, #123): see console_capture.h
#include "console_capture.h"

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
