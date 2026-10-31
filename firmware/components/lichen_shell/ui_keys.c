#include "ui_keys.h"

#include <stdbool.h>
#include <string.h>

// LVGL's key codes (ui_probe.c static_asserts them)
static struct {
    char const *name;
    uint32_t    key;
} const NAMES[] = {{"up", 17},   {"down", 18}, {"left", 20}, {"right", 19}, {"enter", 10},
                   {"esc", 27},  {"tab", 9},   {"bksp", 8},  {"wait", UI_KEY_WAIT}};

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
