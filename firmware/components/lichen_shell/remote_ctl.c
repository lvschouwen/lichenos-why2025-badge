#include "remote_ctl.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void rlock_on(rlock_t *l, uint32_t seconds, uint32_t now) {
    if (seconds == 0 || seconds > RLOCK_MAX_S) {
        seconds = RLOCK_DEFAULT_S;
    }
    if (!l->on) {
        l->esc = (hold_exit_t){0}; // an Esc already down when the lock starts is no escape
    }
    l->on         = true;
    l->timeout_ms = seconds * 1000u;
    l->last_ms    = now;
    l->ended      = RLOCK_END_NONE;
}

static void end(rlock_t *l, rlock_end_t why) {
    l->on    = false;
    l->esc   = (hold_exit_t){0};
    l->ended = why;
}

void rlock_off(rlock_t *l) {
    if (l->on) {
        end(l, RLOCK_END_CONSOLE);
    }
}

void rlock_touch(rlock_t *l, uint32_t now) {
    l->last_ms = now;
}

bool rlock_tick(rlock_t *l, uint32_t now) {
    if (!l->on) {
        return false;
    }
    if ((uint32_t)(now - l->last_ms) >= l->timeout_ms) {
        end(l, RLOCK_END_TIMEOUT);
    } else if (hold_exit_tick_after(&l->esc, now, RLOCK_ESC_HOLD_MS)) {
        end(l, RLOCK_END_ESC);
    }
    return l->on;
}

bool rlock_key(rlock_t *l, uint8_t code, bool is_esc, bool pressed, uint32_t now) {
    if (code >= RLOCK_CODES) {
        return l->on;
    }
    // The time may have run out, or Esc reached its hold, since the last tick: this event is judged after that. An
    // Esc released after the full hold therefore still ends the lock if no tick saw it held.
    rlock_tick(l, now);
    if (!pressed && l->dropped[code]) {
        l->dropped[code] = false; // its press was dropped: so is the release, also once the lock is over
        if (l->on && is_esc) {
            hold_exit_event(&l->esc, true, false, now);
        }
        return true;
    }
    if (pressed) {
        l->dropped[code] = false; // a release that never came (lost, or the chip was reset) leaves no stale flag
    }
    if (!l->on) {
        return false;
    }
    if (!pressed) {
        return false; // pressed before the lock: LVGL and the hooks must see it released
    }
    l->dropped[code] = true;
    hold_exit_event(&l->esc, is_esc, true, now);
    return true;
}

void rlock_keys_reset(rlock_t *l) {
    memset(l->dropped, 0, sizeof(l->dropped));
    l->esc = (hold_exit_t){0};
}

uint32_t rlock_left_s(rlock_t const *l, uint32_t now) {
    if (!l->on) {
        return 0;
    }
    uint32_t gone = now - l->last_ms;
    if (gone >= l->timeout_ms) {
        return 0;
    }
    return (l->timeout_ms - gone + 999) / 1000;
}

rlock_end_t rlock_take_end(rlock_t *l) {
    rlock_end_t e = l->ended;
    l->ended      = RLOCK_END_NONE;
    return e;
}

char const *rlock_end_name(rlock_end_t e) {
    switch (e) {
        case RLOCK_END_TIMEOUT: return "timeout";
        case RLOCK_END_ESC: return "Esc held";
        case RLOCK_END_CONSOLE: return "console";
        default: return "none";
    }
}

static char const *skip_spaces(char const *p) {
    while (*p == ' ') {
        p++;
    }
    return p;
}

// The word at p (up to a space or the end) equals word
static bool word_is(char const *p, char const *word, char const **after) {
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0 || (p[n] != '\0' && p[n] != ' ')) {
        return false;
    }
    *after = skip_spaces(p + n);
    return true;
}

rlock_args_t rlock_parse(char const *args, uint32_t *seconds) {
    char const *p = skip_spaces(args);
    *seconds      = RLOCK_DEFAULT_S;
    if (!*p) {
        return RLOCK_ARGS_SHOW;
    }
    if (word_is(p, "off", &p)) {
        return *p ? RLOCK_ARGS_ERR : RLOCK_ARGS_OFF;
    }
    if (!word_is(p, "on", &p)) {
        return RLOCK_ARGS_ERR;
    }
    if (!*p) {
        return RLOCK_ARGS_ON;
    }
    uint32_t value = 0;
    size_t   digits = 0;
    for (; isdigit((unsigned char)*p) && digits < 4; p++, digits++) {
        value = value * 10 + (uint32_t)(*p - '0');
    }
    if (!digits || *skip_spaces(p) || value < 1 || value > RLOCK_MAX_S) {
        return RLOCK_ARGS_ERR;
    }
    *seconds = value;
    return RLOCK_ARGS_ON;
}

// a (length n) equals b ignoring case
static bool same_text(char const *a, size_t n, char const *b) {
    if (!b || strlen(b) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

int rctl_find_app(char const *arg, char const *const *ids, char const *const *names, size_t n) {
    char const *p   = skip_spaces(arg);
    size_t      len = strlen(p);
    while (len && p[len - 1] == ' ') {
        len--;
    }
    if (!len) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        if (same_text(p, len, ids[i])) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (same_text(p, len, names[i])) {
            return (int)i;
        }
    }
    return -1;
}
