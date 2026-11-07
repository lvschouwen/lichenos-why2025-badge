// The gate's font slots (#16, #220): one per LICHEN_FONT_* size, the asset font when lichen_os loaded one
// (font_slots.h), else LVGL's built-in Montserrat. Loading, its PSRAM figures and "fw fonts" are lichen_os/fonts.c's.
#include "fonts.h"
#include "font_slots.h"

#include <stddef.h>

typedef struct {
    int              px;
    lv_font_t const *builtin;
    lv_font_t const *loaded; // NULL: use the built-in one
} slot_t;

static slot_t slots[] = {
    {LICHEN_FONT_SMALL, &lv_font_montserrat_24, NULL},
    {LICHEN_FONT_MEDIUM, &lv_font_montserrat_28, NULL},
    {LICHEN_FONT_LARGE, &lv_font_montserrat_40, NULL},
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

static slot_t *slot_for(int px) {
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        if (slots[i].px == px) {
            return &slots[i];
        }
    }
    return NULL;
}

lv_font_t const *lichen_font(int px) {
    slot_t *s = slot_for(px);
    if (!s) {
        s = &slots[0];
    }
    return s->loaded ? s->loaded : s->builtin;
}

bool lichen_font_set(int px, lv_font_t const *font) {
    slot_t *s = slot_for(px);
    if (!s) {
        return false;
    }
    s->loaded = font;
    return true;
}
