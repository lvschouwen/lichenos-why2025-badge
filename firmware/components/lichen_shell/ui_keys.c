#include "ui_keys.h"

#include <stdint.h>
#include <string.h>

// LVGL's key codes (ui_probe.c static_asserts them); the shape keys as their TCA8418 codes (tca8418_keymap.h)
static struct {
    char const *name;
    uint32_t    key;
} const NAMES[] = {{"up", 17},   {"down", 18}, {"left", 20}, {"right", 19}, {"enter", 10},
                   {"esc", 27},  {"tab", 9},   {"bksp", 8},  {"wait", UI_KEY_WAIT}, {"needlock", UI_KEY_NEEDLOCK},
                   {"phys", UI_KEY_PHYS},
                   {"square", UI_KEY_SHAPE(2)}, {"triangle", UI_KEY_SHAPE(3)}, {"cross", UI_KEY_SHAPE(4)},
                   {"circle", UI_KEY_SHAPE(5)}, {"cloud", UI_KEY_SHAPE(6)},    {"diamond", UI_KEY_SHAPE(7)}};

static uint32_t guard_value(uint32_t kind, size_t offset, size_t len) {
    return UI_KEY_GUARD_BIT | kind | ((uint32_t)len << 12) | (uint32_t)offset;
}

static bool prefix(char const *p, char const *word, size_t *skip) {
    size_t n = strlen(word);
    if (strncmp(p, word, n) != 0) {
        return false;
    }
    *skip = n;
    return true;
}

static bool is_guard_token(char const *p) {
    size_t skip;
    return prefix(p, "row:", &skip) || prefix(p, "focus:", &skip) || prefix(p, "notfocus:", &skip);
}

// "focus:" / "row:" / "notfocus:" at p: one guard key for the text after it (up to '|' or the end). Returns the end of the token,
// NULL when p is no guard or the text is empty or too long.
static char const *parse_guard(char const *seq, char const *p, uint32_t *out) {
    size_t   skip = 0;
    uint32_t kind = 0;
    if (prefix(p, "row:", &skip)) {
        kind = UI_KEY_GUARD_EXACT;
    } else if (prefix(p, "notfocus:", &skip)) {
        kind = UI_KEY_GUARD_NOT;
    } else if (!prefix(p, "focus:", &skip)) {
        return NULL;
    }
    char const *text = p + skip;
    size_t      len  = strcspn(text, "|");
    size_t      off  = (size_t)(text - seq);
    if (!len || len > UI_KEY_GUARD_MAX_TEXT || off > UI_KEY_GUARD_MAX_TEXT) {
        return NULL;
    }
    *out = guard_value(kind, off, len);
    return text + len;
}

// want (len bytes, not terminated) occurs in focus
static bool contains(char const *focus, char const *want, size_t len) {
    for (char const *f = focus; strlen(f) >= len; f++) {
        if (strncmp(f, want, len) == 0) {
            return true;
        }
    }
    return false;
}

// None of the ';'-separated texts in want occurs in focus
static bool contains_none(char const *focus, char const *want, size_t len) {
    char const *end = want + len;
    while (want < end) {
        char const *semi = memchr(want, ';', (size_t)(end - want));
        size_t      n    = (size_t)((semi ? semi : end) - want);
        if (n && contains(focus, want, n)) {
            return false;
        }
        want += n + 1;
    }
    return true;
}

bool ui_keys_guard_ok(uint32_t guard, char const *seq, char const *focus) {
    if (!UI_KEY_IS_GUARD(guard) || !focus || !*focus || strcmp(focus, "(none)") == 0) {
        return false;
    }
    char const *want = seq + (guard & UI_KEY_GUARD_MAX_TEXT);
    size_t      len  = (guard >> 12) & UI_KEY_GUARD_MAX_TEXT;
    if (guard & UI_KEY_GUARD_NOT) {
        return contains_none(focus, want, len);
    }
    if (!(guard & UI_KEY_GUARD_EXACT)) {
        return contains(focus, want, len);
    }
    while (*focus == '*' || *focus == ' ') {
        focus++; // a checked row's "* " (ui_probe.c)
    }
    size_t flen = strlen(focus);
    while (flen && focus[flen - 1] == ' ') {
        flen--;
    }
    return flen == len && strncmp(focus, want, len) == 0;
}

// The NAMES entry spelled by the len characters at p
static bool named_key(char const *p, size_t len, uint32_t *key) {
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strlen(NAMES[i].name) == len && strncmp(p, NAMES[i].name, len) == 0) {
            *key = NAMES[i].key;
            return true;
        }
    }
    return false;
}

bool ui_keys_is_key(uint32_t k) {
    return k != UI_KEY_WAIT && k != UI_KEY_NEEDLOCK && k != UI_KEY_PHYS && !UI_KEY_IS_GUARD(k) && !UI_KEY_IS_HOLD(k);
}

uint32_t ui_keys_wait_ms(uint32_t now, uint32_t next_at, bool keys_left, uint32_t const *until, size_t n_held) {
    int32_t gap  = (int32_t)(next_at - now);
    int32_t wait = keys_left || gap > 0 || !n_held ? gap : INT32_MAX;
    for (size_t i = 0; i < n_held; i++) {
        int32_t d = (int32_t)(until[i] - now);
        wait      = d < wait ? d : wait;
    }
    return wait < 1 ? 1 : (uint32_t)wait;
}

// "hold:<ms>" at p: false unless ms is digits up to the next ' ' or '|'. *end is past them; more than the cap is capped.
static bool parse_hold_ms(char const *p, uint32_t *ms, char const **end) {
    size_t skip;
    if (!prefix(p, "hold:", &skip)) {
        return false;
    }
    p += skip;
    size_t len = strcspn(p, " |");
    if (!len) {
        return false;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        v = v > UI_KEY_HOLD_MAX_MS ? v : v * 10 + (uint32_t)(p[i] - '0'); // capped: no overflow on long strings
    }
    *ms  = v > UI_KEY_HOLD_MAX_MS ? UI_KEY_HOLD_MAX_MS : v;
    *end = p + len;
    return true;
}

// Puts the hold right after the last key (over any phys); false when there is no key before it or out is full
static bool add_hold(uint32_t *out, size_t *n, size_t max, uint32_t ms) {
    size_t at = *n;
    while (at && out[at - 1] == UI_KEY_PHYS) {
        at--;
    }
    if (!at || !ui_keys_is_key(out[at - 1]) || *n >= max) {
        return false;
    }
    memmove(&out[at + 1], &out[at], (*n - at) * sizeof(out[0]));
    out[at] = UI_KEY_HOLD_BIT | ms;
    (*n)++;
    return true;
}

size_t ui_keys_parse(char const *seq, uint32_t *out, size_t max, size_t *err_pos) {
    size_t n = 0;
    char const *p = seq;
    while (*p) {
        if (*p == ' ' || *p == '|') {
            p++;
            continue;
        }
        if (strncmp(p, "text:", 5) == 0) {
            char const *start = p;
            p += 5;
            while (*p && *p != '|') {
                if (n >= max) {
                    *err_pos = (size_t)(start - seq);
                    return UI_KEYS_ERR;
                }
                out[n++] = (uint8_t)*p++;
            }
            continue;
        }
        if (is_guard_token(p)) {
            uint32_t    guard = 0;
            char const *end   = parse_guard(seq, p, &guard);
            if (!end || n >= max) {
                *err_pos = (size_t)(p - seq);
                return UI_KEYS_ERR;
            }
            out[n++] = guard;
            p        = end;
            continue;
        }
        uint32_t    ms;
        char const *end;
        if (parse_hold_ms(p, &ms, &end)) {
            if (!add_hold(out, &n, max, ms)) {
                *err_pos = (size_t)(p - seq);
                return UI_KEYS_ERR;
            }
            p = end;
            continue;
        }
        size_t len = strcspn(p, " |");
        if (n >= max || !named_key(p, len, &out[n])) {
            *err_pos = (size_t)(p - seq);
            return UI_KEYS_ERR;
        }
        n++;
        p += len;
    }
    return n;
}
