#include "console_cmd.h"

#include <string.h>

static size_t const PREFIX_LEN = sizeof(CONSOLE_CMD_PREFIX) - 1;

static bool is_newline(char c) {
    return c == '\n' || c == '\r';
}

void console_cmd_init(console_cmd_t *s) {
    memset(s, 0, sizeof *s);
    s->at_line_start = true;
}

static console_cmd_result_t pass_held(console_cmd_t *s, char const **pass, size_t *pass_len) {
    *pass     = s->buf;
    *pass_len = s->len;
    s->len    = 0;
    return CONSOLE_CMD_PASS;
}

console_cmd_result_t console_cmd_flush(console_cmd_t *s, char const **pass, size_t *pass_len) {
    *pass              = NULL;
    *pass_len          = 0;
    s->swallow_newline = false;
    if (s->in_command || s->len == 0) {
        return CONSOLE_CMD_IDLE;
    }
    // Whatever follows continues this line, so it can't start a command
    s->at_line_start = false;
    return pass_held(s, pass, pass_len);
}

console_cmd_result_t console_cmd_feed(console_cmd_t *s, char c, char const **pass, size_t *pass_len) {
    *pass     = NULL;
    *pass_len = 0;

    if (s->swallow_newline) {
        s->swallow_newline = false;
        if (is_newline(c)) {
            return CONSOLE_CMD_IDLE;
        }
    }

    if (s->in_command) {
        if (is_newline(c)) {
            s->in_command      = false;
            s->at_line_start   = true;
            s->swallow_newline = true;
            if (s->overflow) {
                s->overflow = false;
                s->len      = 0;
                return CONSOLE_CMD_TOO_LONG;
            }
            s->buf[s->len] = '\0';
            s->len         = 0;
            return CONSOLE_CMD_READY;
        }
        if (s->len < sizeof s->buf - 1) {
            s->buf[s->len++] = c;
        } else {
            s->overflow = true;
        }
        return CONSOLE_CMD_IDLE;
    }

    if (!s->at_line_start) {
        // Mid-line bytes can't start a command: straight through
        s->at_line_start = is_newline(c);
        s->buf[0]        = c;
        *pass            = s->buf;
        *pass_len        = 1;
        return CONSOLE_CMD_PASS;
    }

    // At a line start: hold bytes while they still match the prefix
    s->buf[s->len++] = c;
    if (memcmp(s->buf, CONSOLE_CMD_PREFIX, s->len) == 0) {
        if (s->len == PREFIX_LEN) {
            s->in_command    = true;
            s->at_line_start = false;
            s->len           = 0;
        }
        return CONSOLE_CMD_IDLE;
    }

    // Not a command after all: release what was held, the last byte decides where the next line starts
    s->at_line_start = is_newline(c);
    return pass_held(s, pass, pass_len);
}

size_t console_line_max(void) {
    return CONSOLE_WIFI_CMD_MAX;
}

bool console_wifi_len_ok(long content_length) {
    return content_length >= 1 && content_length < CONSOLE_WIFI_CMD_MAX;
}
