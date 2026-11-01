// Host test for Lichen's generated fonts (#16): parses the lv_font_conv binary fonts in firmware/assets/fonts (the
// path is the first argument) and checks that their character maps cover the text the badge shows. The lookup
// mirrors LVGL's get_glyph_dsc_id(), so a code point counts only when LVGL would find a glyph for it.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

typedef struct {
    uint8_t *buf;
    long     size;
    uint32_t cmap_at; // offset of the "cmap" section
    uint32_t count;   // sub-tables
} font_t;

static uint32_t rd32(uint8_t const *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(uint8_t const *p) { return (uint16_t)(p[0] | p[1] << 8); }

static bool font_open(font_t *f, char const *dir, int px) {
    char path[512];
    snprintf(path, sizeof(path), "%s/lichen_%d.bin", dir, px);
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("cannot open %s\n", path);
        return false;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (f->size = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    f->buf = malloc((size_t)f->size);
    bool ok = f->buf && fread(f->buf, 1, (size_t)f->size, fp) == (size_t)f->size;
    fclose(fp);
    if (!ok || f->size < 16 || memcmp(f->buf + 4, "head", 4) != 0) {
        return false;
    }
    f->cmap_at = rd32(f->buf); // the head section is first, the cmap follows it
    if ((long)f->cmap_at + 12 > f->size || memcmp(f->buf + f->cmap_at + 4, "cmap", 4) != 0) {
        return false;
    }
    f->count = rd32(f->buf + f->cmap_at + 8);
    return (long)f->cmap_at + 12 + (long)f->count * 16 <= f->size;
}

// A cmap sub-table header: data_offset u32, range_start u32, range_length u16, glyph_id_start u16,
// data_entries_count u16, format u8, pad u8. Formats: 0 format0 full, 1 sparse full, 2 format0 tiny, 3 sparse tiny.
static bool has_glyph(font_t const *f, uint32_t cp) {
    for (uint32_t i = 0; i < f->count; i++) {
        uint8_t const *t      = f->buf + f->cmap_at + 12 + i * 16;
        uint32_t       data   = f->cmap_at + rd32(t);
        uint32_t       start  = rd32(t + 4);
        uint32_t       rcp    = cp - start;
        uint32_t       length = rd16(t + 8);
        uint32_t       n      = rd16(t + 12);
        uint8_t        format = t[14];
        if (rcp >= length) {
            continue;
        }
        if (format == 2) {
            return true;
        }
        if (format == 0) {
            return f->buf[data + rcp] != 0 || cp == start;
        }
        for (uint32_t k = 0; k < n; k++) { // sparse: a list of 16-bit offsets from range_start
            if (rd16(f->buf + data + 2 * k) == rcp) {
                return true;
            }
        }
        return false;
    }
    return false;
}

// Next code point of a UTF-8 string (the samples are valid)
static uint32_t next_cp(char const **s) {
    uint8_t const *p = (uint8_t const *)*s;
    uint32_t       c = *p;
    int            extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
    for (int i = 1; i <= extra; i++) {
        c = c << 6 | (p[i] & 0x3F);
    }
    *s += 1 + extra;
    return c;
}

static int missing(font_t const *f, char const *text) {
    int n = 0;
    for (char const *s = text; *s;) {
        uint32_t c = next_cp(&s);
        if (!has_glyph(f, c)) {
            printf("  U+%04X missing\n", c);
            n++;
        }
    }
    return n;
}

// Text from the field and from #16: Latin-1/Extended, typographic punctuation, euro, Cyrillic, Greek; the emoji in
// the 36 contact names (the U+1F900 block ones last) with a sun's variation selector; ZWJ and a zero-width space
static char const *const SAMPLES[] = {
    "Hello, World! 0123456789 ~ #",
    "\xc3\xa9 \xc3\xbc \xc3\x9f \xc3\xa0 \xc3\xb1 \xc3\x85 \xc4\x8d \xc5\x82 \xc5\xa1 \xc5\xbe",
    "\xe2\x80\x99 \xe2\x80\x9c \xe2\x80\x9d \xe2\x80\x93 \xe2\x80\x94 \xe2\x80\xa6 \xe2\x80\xa2 \xe2\x82\xac",
    "\xd0\x96 \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xd0\x81 \xd1\x91",
    "\xce\xbb \xce\xa9 \xce\xb1\xce\xb2\xce\xb3 \xcf\x80",
    "\xe2\x86\x90 \xe2\x86\x92 \xe2\x96\xa0 \xe2\x80\x8b",
    "\xe2\x98\x80\xef\xb8\x8f \xf0\x9f\x8d\x84 \xf0\x9f\x90\x8c \xf0\x9f\x91\x80 \xf0\x9f\x94\x8b \xf0\x9f\x94\xb1",
    "\xf0\x9f\xa4\x96 \xf0\x9f\xa6\x8b \xf0\x9f\xa6\x8e \xf0\x9f\xa6\xab",
    "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9 \xe2\x9c\x8c\xef\xb8\x8e",
    "\xe2\xad\x90", // U+2B50, a favourite in MeshCore's Contacts (#49); U+2605 is not in the fonts
};

// LV_SYMBOL_* code points (the first and last of the list the fonts are made with)
static uint32_t const SYMBOLS[] = {0xF001, 0xF00B, 0xF013, 0xF1EB, 0xF240, 0xF7C2};

static void test_font(char const *dir, int px) {
    font_t f = {0};
    printf("lichen_%d.bin\n", px);
    bool const opened = font_open(&f, dir, px);
    CHECK(opened);
    if (!opened) { // cmap_at/count are not validated: don't walk them
        free(f.buf);
        return;
    }
    for (size_t i = 0; i < sizeof(SAMPLES) / sizeof(SAMPLES[0]); i++) {
        CHECK(missing(&f, SAMPLES[i]) == 0);
    }
    for (size_t i = 0; i < sizeof(SYMBOLS) / sizeof(SYMBOLS[0]); i++) {
        CHECK(has_glyph(&f, SYMBOLS[i]));
    }
    // The lookup is not just saying yes: CJK and an unassigned block are outside every range
    CHECK(!has_glyph(&f, 0x4E2D));
    CHECK(!has_glyph(&f, 0x0600));
    CHECK(!has_glyph(&f, 0x7F));
    free(f.buf);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: font_cover_test <dir with lichen_<px>.bin>\n");
        return 2;
    }
    test_font(argv[1], 24);
    test_font(argv[1], 28);
    test_font(argv[1], 40);
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
