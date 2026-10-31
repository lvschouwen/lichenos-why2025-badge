#include "fonts.h"

#include "assets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fw_console.h"

#include <string.h>

static char const *TAG = "fonts";

typedef struct {
    int                    px;
    char const            *file;
    lv_font_t const       *builtin;
    lv_font_t             *loaded; // NULL: use the built-in one
    uint32_t               ram;    // PSRAM the loaded font took
} slot_t;

static slot_t slots[] = {
    {LICHEN_FONT_SMALL, "fonts/lichen_24.bin", &lv_font_montserrat_24, NULL, 0},
    {LICHEN_FONT_MEDIUM, "fonts/lichen_28.bin", &lv_font_montserrat_28, NULL, 0},
    {LICHEN_FONT_LARGE, "fonts/lichen_40.bin", &lv_font_montserrat_40, NULL, 0},
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

// Every kind of character the mesh shows: node names with emoji (U+1F900 block, a sun with its zero-width variation
// selector), the accents and punctuation of other languages, symbols
static char const *const SAMPLE = "\xc3\xa9 \xc3\xbc \xc3\x9f \xe2\x80\x99 \xe2\x80\x9c \xe2\x80\x9d \xe2\x80\x93 \xe2\x80\x94 \xe2\x80\xa6 "
                                  "\xe2\x82\xac \xd0\x96 \xce\xbb \xe2\x98\x80\xef\xb8\x8f \xf0\x9f\x8d\x84 \xf0\x9f\x90\x8c "
                                  "\xf0\x9f\x91\x80 \xf0\x9f\x94\x8b \xf0\x9f\x94\xb1 \xf0\x9f\xa4\x96 \xf0\x9f\xa6\x8b "
                                  "\xf0\x9f\xa6\x8e \xf0\x9f\xa6\xab \xe2\x80\x8d " LV_SYMBOL_WIFI LV_SYMBOL_BATTERY_FULL;

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

// lv_binfont_create_from_buffer parses the file into its own lv_malloc'd (PSRAM) structures; nothing keeps pointing
// into the mapped partition, which a Wi-Fi assets update rewrites (assets.h).
static void load_one(slot_t *s) {
    uint8_t const *data;
    uint32_t       size;
    if (!assets_get(s->file, &data, &size)) {
        ESP_LOGW(TAG, "%s missing, built-in %d px font", s->file, s->px);
        return;
    }
    size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->loaded     = lv_binfont_create_from_buffer((void *)data, size);
    if (!s->loaded) {
        ESP_LOGW(TAG, "%s damaged, built-in %d px font", s->file, s->px);
        return;
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s->ram       = before > after ? (uint32_t)(before - after) : 0;
}

// Next code point of a valid UTF-8 string (the sample is ours), advancing *s
static uint32_t next_cp(char const **s) {
    uint8_t const *p     = (uint8_t const *)*s;
    int            extra = *p >= 0xF0 ? 3 : *p >= 0xE0 ? 2 : *p >= 0xC0 ? 1 : 0;
    uint32_t       c     = *p & (0x7F >> extra);
    for (int i = 1; i <= extra; i++) {
        c = c << 6 | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return c;
}

static unsigned missing_in(lv_font_t const *font, char const *text) {
    unsigned missing = 0;
    for (char const *s = text; *s;) {
        uint32_t            c = next_cp(&s);
        lv_font_glyph_dsc_t g;
        if (c != ' ' && !lv_font_get_glyph_dsc(font, &g, c, 0)) {
            missing++;
        }
    }
    return missing;
}

static void cmd_fonts(char const *args) {
    bool cover = strcmp(args, "cover") == 0;
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        slot_t *s = &slots[i];
        if (cover) {
            console_reply("fonts %d px: %u of the sample's characters missing", s->px, missing_in(lichen_font(s->px), SAMPLE));
        } else {
            console_reply("fonts %d px: %s, %u bytes PSRAM, line %d", s->px, s->loaded ? "custom" : "built-in", (unsigned)s->ram,
                          (int)lv_font_get_line_height(lichen_font(s->px)));
        }
    }
}

void lichen_fonts_load(void) {
    unsigned loaded = 0;
    unsigned bytes  = 0;
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        load_one(&slots[i]);
        loaded += slots[i].loaded != NULL;
        bytes += slots[i].ram;
    }
    // Warning level on purpose: the badge's default log level hides info lines and this is the one to look for
    ESP_LOGW(TAG, "%u of %u fonts loaded, %u KB PSRAM%s", loaded, (unsigned)SLOT_COUNT, bytes / 1024,
             loaded == SLOT_COUNT ? "" : " (the rest is built-in)");
    console_register("fonts", "fonts [cover] (which fonts are active and their PSRAM size, or sample-text coverage)", cmd_fonts);
}
