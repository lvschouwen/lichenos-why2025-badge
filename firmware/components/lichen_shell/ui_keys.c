#include "ui_keys.h"

#include <string.h>

// LVGL's key codes (ui_probe.c static_asserts them); the shape keys as their TCA8418 codes (tca8418_keymap.h)
static struct {
    char const *name;
    uint32_t    key;
} const NAMES[] = {{"up", 17},   {"down", 18}, {"left", 20}, {"right", 19}, {"enter", 10},
                   {"esc", 27},  {"tab", 9},   {"bksp", 8},  {"wait", UI_KEY_WAIT},
                   {"square", UI_KEY_SHAPE(2)}, {"triangle", UI_KEY_SHAPE(3)}, {"cross", UI_KEY_SHAPE(4)},
                   {"circle", UI_KEY_SHAPE(5)}, {"cloud", UI_KEY_SHAPE(6)},    {"diamond", UI_KEY_SHAPE(7)}};

static uint32_t guard_value(bool exact, size_t offset, size_t len) {
    return UI_KEY_GUARD_BIT | (exact ? UI_KEY_GUARD_EXACT : 0) | ((uint32_t)len << 12) | (uint32_t)offset;
}

// "focus:" / "row:" at p: one guard key for the text after it (up to '|' or the end). Returns the end of the token,
// NULL when p is no guard or the text is empty or too long.
static char const *parse_guard(char const *seq, char const *p, uint32_t *out) {
    bool   exact = strncmp(p, "row:", 4) == 0;
    size_t skip  = exact ? 4 : (strncmp(p, "focus:", 6) == 0 ? 6 : 0);
    if (!skip) {
        return NULL;
    }
    char const *text = p + skip;
    size_t      len  = strcspn(text, "|");
    size_t      off  = (size_t)(text - seq);
    if (!len || len > UI_KEY_GUARD_MAX_TEXT || off > UI_KEY_GUARD_MAX_TEXT) {
        return NULL;
    }
    *out = guard_value(exact, off, len);
    return text + len;
}

bool ui_keys_guard_ok(uint32_t guard, char const *seq, char const *focus) {
    if (!UI_KEY_IS_GUARD(guard) || !focus || !*focus || strcmp(focus, "(none)") == 0) {
        return false;
    }
    char const *want = seq + (guard & UI_KEY_GUARD_MAX_TEXT);
    size_t      len  = (guard >> 12) & UI_KEY_GUARD_MAX_TEXT;
    if (!(guard & UI_KEY_GUARD_EXACT)) {
        for (char const *f = focus; strlen(f) >= len; f++) {
            if (strncmp(f, want, len) == 0) {
                return true;
            }
        }
        return false;
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
        if (strncmp(p, "focus:", 6) == 0 || strncmp(p, "row:", 4) == 0) {
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
        size_t len = strcspn(p, " |");
        bool   hit = false;
        for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
            if (strlen(NAMES[i].name) == len && strncmp(p, NAMES[i].name, len) == 0) {
                if (n >= max) {
                    *err_pos = (size_t)(p - seq);
                    return UI_KEYS_ERR;
                }
                out[n++] = NAMES[i].key;
                hit      = true;
                break;
            }
        }
        if (!hit) {
            *err_pos = (size_t)(p - seq);
            return UI_KEYS_ERR;
        }
        p += len;
    }
    return n;
}
