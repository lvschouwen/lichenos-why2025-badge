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

bool console_c6_id_valid(char const *id) {
    size_t len = 0;
    for (; id[len]; len++) {
        char c = id[len];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') || len == CONSOLE_C6_ID_MAX) {
            return false;
        }
    }
    return len > 0;
}

bool console_c6log_ms(char const *arg, uint32_t *ms) {
    if (!*arg) {
        *ms = CONSOLE_C6LOG_DEFAULT_MS;
        return true;
    }
    uint32_t v = 0;
    for (char const *c = arg; *c; c++) {
        if (*c < '0' || *c > '9' || c - arg >= 5) {
            return false;
        }
        v = v * 10 + (uint32_t)(*c - '0');
    }
    if (v == 0 || v > CONSOLE_C6LOG_MAX_MS) {
        return false;
    }
    *ms = v;
    return true;
}

// Digits up to `end` (or the string's end) as a number of at most `max`; false on anything else
static bool parse_bounded(char const *s, char const *end, uint32_t max, uint32_t *out) {
    if (s == end || !*s) {
        return false;
    }
    uint64_t v = 0;
    for (char const *c = s; c != end && *c; c++) {
        if (*c < '0' || *c > '9' || c - s >= 10) {
            return false;
        }
        v = v * 10 + (uint64_t)(*c - '0');
    }
    if (v > max) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

bool console_memhog_args(char const *arg, uint32_t *left, uint32_t *secs) {
    char const *space = strchr(arg, ' ');
    if (!space) {
        return false;
    }
    uint32_t l, s;
    if (!parse_bounded(arg, space, CONSOLE_MEMHOG_MAX_LEFT, &l) ||
        !parse_bounded(space + 1, NULL, CONSOLE_MEMHOG_MAX_S, &s) || s == 0) {
        return false;
    }
    *left = l;
    *secs = s;
    return true;
}
