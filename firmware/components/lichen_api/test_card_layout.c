#include "test_card_layout.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Bands from the top (720 px): info 10..73, colour bars 86..165, middle band 180..539 (gratings left and right of the
// circle), wedge 556..615, marker 640..663. Nothing overlaps, and the grid shows behind the gaps.
#define BAR_X     60
#define BAR_W     75
#define BAR_Y     86
#define BAR_H     80
#define WEDGE_X   57
#define WEDGE_W   55
#define WEDGE_Y   556
#define WEDGE_H   60
#define MARK_X    24
#define MARK_PITCH 28
#define MARK_W    24
#define MARK_Y    640
#define MARK_H    24
#define CORNER_INSET 4
#define CORNER_ARM   28
#define CORNER_THICK 3
#define GRAT_SIZE 120

static bool word_is(char const *s, char const *word) {
    while (*s == ' ') {
        s++;
    }
    size_t n = strlen(word);
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)s[i]) != word[i]) {
            return false;
        }
    }
    for (s += n; *s == ' '; s++) {
    }
    return *s == '\0';
}

bool tcard_mode_parse(char const *args, tcard_mode_t *mode) {
    static struct {
        char const  *word;
        tcard_mode_t mode;
    } const names[] = {{"", TCARD_TEST},         {"test", TCARD_TEST},   {"white", TCARD_WHITE},
                       {"grey", TCARD_GREY},     {"gray", TCARD_GREY},   {"black", TCARD_BLACK},
                       {"off", TCARD_OFF}};
    if (!args) {
        args = "";
    }
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (word_is(args, names[i].word)) {
            *mode = names[i].mode;
            return true;
        }
    }
    return false;
}

tcard_mode_t tcard_mode_step(tcard_mode_t mode, int delta) {
    int const n = TCARD_BLACK + 1; // the four fields the app cycles
    int       m = (int)mode;
    if (m >= n) {
        m = 0;
    }
    return (tcard_mode_t)(((m + delta) % n + n) % n);
}

uint32_t tcard_solid_color(tcard_mode_t mode) {
    switch (mode) {
    case TCARD_WHITE:
        return 0xFFFFFF;
    case TCARD_GREY:
        return 0x808080;
    default:
        return 0; // black, and the modes that draw no field
    }
}

tcard_rect_t tcard_info_rect(void) {
    return (tcard_rect_t){40, 10, 640, 64};
}
tcard_rect_t tcard_circle_rect(void) {
    return (tcard_rect_t){180, 180, 360, 360};
}
tcard_rect_t tcard_border_rect(void) {
    return (tcard_rect_t){0, 0, TCARD_SIZE, TCARD_SIZE};
}

tcard_rect_t tcard_corner_rect(int i) {
    int const far = TCARD_SIZE - CORNER_INSET - CORNER_ARM; // x or y of the far corners' arm start
    int const edge = TCARD_SIZE - CORNER_INSET - CORNER_THICK;
    int const c = (i / 2) % 4;
    bool const right = (c & 1) != 0, bottom = (c & 2) != 0;
    bool const second = (i & 1) != 0; // first arm runs along x, second along y
    int x, y, w, h;
    if (!second) {
        x = right ? far : CORNER_INSET;
        y = bottom ? edge : CORNER_INSET;
        w = CORNER_ARM;
        h = CORNER_THICK;
    } else {
        x = right ? edge : CORNER_INSET;
        y = bottom ? far : CORNER_INSET;
        w = CORNER_THICK;
        h = CORNER_ARM;
    }
    return (tcard_rect_t){(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h};
}

tcard_rect_t tcard_grid_rect(int i, bool vertical) {
    int const at = (i + 1) * TCARD_GRID_STEP;
    return vertical ? (tcard_rect_t){(int16_t)at, 0, 1, TCARD_SIZE} : (tcard_rect_t){0, (int16_t)at, TCARD_SIZE, 1};
}

tcard_rect_t tcard_bar_rect(int i) {
    return (tcard_rect_t){(int16_t)(BAR_X + i * BAR_W), BAR_Y, BAR_W, BAR_H};
}

uint32_t tcard_bar_color(int i) {
    static uint32_t const colors[TCARD_BARS] = {0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF,
                                                0xFF00FF, 0xFFFF00, 0xFFFFFF, 0x000000};
    return colors[i];
}

tcard_rect_t tcard_wedge_rect(int i) {
    return (tcard_rect_t){(int16_t)(WEDGE_X + i * WEDGE_W), WEDGE_Y, WEDGE_W, WEDGE_H};
}

int tcard_wedge_percent(int i) {
    return i * 100 / (TCARD_WEDGE - 1);
}

uint32_t tcard_wedge_color(int i) {
    uint32_t g = (uint32_t)(tcard_wedge_percent(i) * 255 / 100);
    return (g << 16) | (g << 8) | g;
}

tcard_rect_t tcard_marker_track(void) {
    return (tcard_rect_t){MARK_X, MARK_Y + MARK_H / 2 - 1, (TCARD_MARKER_POS - 1) * MARK_PITCH + MARK_W, 2};
}

tcard_rect_t tcard_marker_rect(uint32_t frame) {
    return (tcard_rect_t){(int16_t)(MARK_X + (int)(frame % TCARD_MARKER_POS) * MARK_PITCH), MARK_Y, MARK_W, MARK_H};
}

tcard_grating_t tcard_grating(int i) {
    // 0 and 1 (top, left and right of the circle) are vertical lines of 1 and 2 px, 2 and 3 (below them) horizontal ones
    int const x = (i & 1) ? 570 : 30;
    int const y = (i & 2) ? 401 : 200;
    bool const vertical = i < 2;
    int const line = (i & 1) ? 2 : 1;
    return (tcard_grating_t){{(int16_t)x, (int16_t)y, GRAT_SIZE, GRAT_SIZE}, vertical, line};
}

bool tcard_grating_line(tcard_grating_t const *g, int i, tcard_rect_t *out) {
    int const off = i * 2 * g->line_px;
    int const span = g->vertical ? g->area.w : g->area.h;
    if (i < 0 || off + g->line_px > span) {
        return false;
    }
    *out = g->vertical ? (tcard_rect_t){(int16_t)(g->area.x + off), g->area.y, (int16_t)g->line_px, g->area.h}
                       : (tcard_rect_t){g->area.x, (int16_t)(g->area.y + off), g->area.w, (int16_t)g->line_px};
    return true;
}

// The value of a registry text: the part before the first space; "?" when unread, "" when the key is absent
static void reg_value(char const *raw, char *out, size_t cap) {
    size_t n = 0;
    if (raw) {
        while (raw[n] && raw[n] != ' ' && n + 1 < cap) {
            out[n] = raw[n];
            n++;
        }
    }
    out[n] = '\0';
    if (n == 0 || out[0] == '?') {
        snprintf(out, cap, "?");
    }
}

static void refresh_text(char const *raw, char *out, size_t cap) {
    char v[16];
    reg_value(raw, v, sizeof(v));
    unsigned dhz = 0;
    if (v[0] == '?' || sscanf(v, "%u", &dhz) != 1) {
        snprintf(out, cap, "? Hz");
        return;
    }
    snprintf(out, cap, "%u.%u Hz", dhz / 10, dhz % 10);
}

// psram.clk, Hz, as "120 MHz"; "? MHz" when unread
static void psram_text(char const *raw, char *out, size_t cap) {
    char v[16];
    reg_value(raw, v, sizeof(v));
    unsigned long hz = 0;
    if (v[0] == '?' || sscanf(v, "%lu", &hz) != 1) {
        snprintf(out, cap, "? MHz");
    } else {
        snprintf(out, cap, "%lu MHz", (hz + 500000) / 1000000);
    }
}

void tcard_info_text(tcard_info_t const *in, char *line1, size_t cap1, char *line2, size_t cap2) {
    char hz[16], d[16], k[16], ps[40];
    psram_text(in->psram_raw, ps, sizeof(ps));
    refresh_text(in->refresh_raw, hz, sizeof(hz));
    reg_value(in->bl_disp_raw, d, sizeof(d));
    reg_value(in->bl_kbd_raw, k, sizeof(k));
    snprintf(line1, cap1, "%s   panel %s   psram %s", in->fw && in->fw[0] ? in->fw : "fw ?", hz, ps);
    if (in->req_disp < 0) { // the caller cannot say what was requested
        snprintf(line2, cap2, "bl applied %s/%s", d, k);
        return;
    }
    snprintf(line2, cap2, "bl requested %d/%d   applied %s/%s", in->req_disp, in->req_kbd, d, k);
}
