// Loads the asset fonts into PSRAM at boot and hands them to the gate's slots (font_slots.h, #220); "fw fonts" reports
// them. lichen_font() itself is the gate's (lichen_api/fonts.c).
#include "fonts.h"
#include "font_slots.h"

#include "assets.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_sys_ui.h"
#include "fw_console.h"

#include <string.h>

// The asset fonts are RLE-compressed (tools/make_fonts.py). Without this option they load fine but
// draw nothing, and the built-in fallback never kicks in.
#if !LV_USE_FONT_COMPRESSED
#error "Lichen fonts need CONFIG_LV_USE_FONT_COMPRESSED=y"
#endif

static char const *TAG = "fonts";

typedef struct {
    int         px;
    char const *file;
    lv_font_t  *loaded; // NULL: the gate's slot keeps the built-in one
    uint32_t    ram;    // PSRAM the loaded font took
} slot_t;

static slot_t slots[] = {
    {LICHEN_FONT_SMALL, "fonts/lichen_24.bin", NULL, 0},
    {LICHEN_FONT_MEDIUM, "fonts/lichen_28.bin", NULL, 0},
    {LICHEN_FONT_LARGE, "fonts/lichen_40.bin", NULL, 0},
};
#define SLOT_COUNT (sizeof(slots) / sizeof(slots[0]))

// Every kind of character the mesh shows: node names with emoji (U+1F900 block, a sun with its zero-width variation
// selector), the accents and punctuation of other languages, symbols
static char const *const SAMPLE = "\xc3\xa9 \xc3\xbc \xc3\x9f \xe2\x80\x99 \xe2\x80\x9c \xe2\x80\x9d \xe2\x80\x93 \xe2\x80\x94 \xe2\x80\xa6 "
                                  "\xe2\x82\xac \xd0\x96 \xce\xbb \xe2\x98\x80\xef\xb8\x8f \xf0\x9f\x8d\x84 \xf0\x9f\x90\x8c "
                                  "\xf0\x9f\x91\x80 \xf0\x9f\x94\x8b \xf0\x9f\x94\xb1 \xf0\x9f\xa4\x96 \xf0\x9f\xa6\x8b "
                                  "\xf0\x9f\xa6\x8e \xf0\x9f\xa6\xab \xe2\x80\x8d " LV_SYMBOL_WIFI LV_SYMBOL_BATTERY_FULL;

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
    lichen_font_set(s->px, s->loaded);
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

// Console task: the glyph lookups write the font's glyph cache, which the LVGL task uses while it renders (#118), so
// the figures are taken under the LVGL lock and printed after it
static void cmd_fonts(char const *args) {
    bool     cover = strcmp(args, "cover") == 0;
    unsigned figure[SLOT_COUNT];
    lichen_sys_ui_lock(0);
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        lv_font_t const *font = lichen_font(slots[i].px);
        figure[i] = cover ? missing_in(font, SAMPLE) : (unsigned)lv_font_get_line_height(font);
    }
    lichen_sys_ui_unlock();
    for (size_t i = 0; i < SLOT_COUNT; i++) {
        slot_t const *s = &slots[i];
        if (cover) {
            console_reply("fonts %d px: %u of the sample's characters missing", s->px, figure[i]);
        } else {
            console_reply("fonts %d px: %s, %u bytes PSRAM, line %u", s->px, s->loaded ? "custom" : "built-in", (unsigned)s->ram,
                          figure[i]);
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
    ESP_LOG_LEVEL(
        loaded == SLOT_COUNT ? ESP_LOG_INFO : ESP_LOG_WARN,
        TAG,
        "%u of %u fonts loaded, %u KB PSRAM%s",
        loaded,
        (unsigned)SLOT_COUNT,
        bytes / 1024,
        loaded == SLOT_COUNT ? "" : " (the rest is built-in)"
    );
    console_register("fonts", "fonts [cover] (which fonts are active and their PSRAM size, or sample-text coverage)", cmd_fonts);
}
