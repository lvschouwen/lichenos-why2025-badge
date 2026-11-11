// Host test: the bench test card's layout, argument and info text (#352)
#include "test_card_layout.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static tcard_mode_t parse(char const *s) {
    tcard_mode_t m = TCARD_MODES;
    return tcard_mode_parse(s, &m) ? m : TCARD_MODES;
}

static void test_parse(void) {
    CHECK(parse("") == TCARD_TEST);
    CHECK(parse("test") == TCARD_TEST);
    CHECK(parse("  White ") == TCARD_WHITE);
    CHECK(parse("grey") == TCARD_GREY && parse("gray") == TCARD_GREY);
    CHECK(parse("black") == TCARD_BLACK);
    CHECK(parse("OFF") == TCARD_OFF);
    CHECK(parse("whit") == TCARD_MODES && parse("white x") == TCARD_MODES && parse("tests") == TCARD_MODES);
    CHECK(tcard_mode_parse(NULL, &(tcard_mode_t){0}));
    CHECK(tcard_solid_color(TCARD_WHITE) == 0xFFFFFF && tcard_solid_color(TCARD_GREY) == 0x808080 &&
          tcard_solid_color(TCARD_BLACK) == 0);
}

static void test_step(void) {
    CHECK(tcard_mode_step(TCARD_TEST, 1) == TCARD_WHITE);
    CHECK(tcard_mode_step(TCARD_WHITE, 1) == TCARD_GREY);
    CHECK(tcard_mode_step(TCARD_GREY, 1) == TCARD_BLACK);
    CHECK(tcard_mode_step(TCARD_BLACK, 1) == TCARD_TEST);
    CHECK(tcard_mode_step(TCARD_TEST, -1) == TCARD_BLACK);
    CHECK(tcard_mode_step(TCARD_GREY, -1) == TCARD_WHITE);
}

static bool inside(tcard_rect_t r) {
    return r.w > 0 && r.h > 0 && r.x >= 0 && r.y >= 0 && r.x + r.w <= TCARD_SIZE && r.y + r.h <= TCARD_SIZE;
}

static bool overlap(tcard_rect_t a, tcard_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

// Every element sits on the panel, and the elements that show a signal do not cover each other
static void test_geometry(void) {
    tcard_rect_t solid[64];
    int          n = 0;
    solid[n++] = tcard_info_rect();
    solid[n++] = tcard_circle_rect();
    for (int i = 0; i < TCARD_BARS; i++) {
        solid[n++] = tcard_bar_rect(i);
    }
    for (int i = 0; i < TCARD_WEDGE; i++) {
        solid[n++] = tcard_wedge_rect(i);
    }
    for (int i = 0; i < TCARD_GRATINGS; i++) {
        solid[n++] = tcard_grating(i).area;
    }
    for (int i = 0; i < n; i++) {
        CHECK(inside(solid[i]));
        for (int j = i + 1; j < n; j++) {
            CHECK(!overlap(solid[i], solid[j]));
        }
    }
    CHECK(inside(tcard_marker_track()));
    for (int f = 0; f < TCARD_MARKER_POS; f++) { // the marker runs along its track, clear of everything else
        tcard_rect_t m = tcard_marker_rect((uint32_t)f);
        CHECK(inside(m) && overlap(m, tcard_marker_track()));
        for (int j = 0; j < n; j++) {
            CHECK(!overlap(m, solid[j]));
        }
    }
    for (int i = 0; i < TCARD_CORNER_RECTS; i++) {
        tcard_rect_t c = tcard_corner_rect(i);
        CHECK(inside(c));
        CHECK(!overlap(c, tcard_info_rect()));
        CHECK(!overlap(c, tcard_marker_track()));
    }
    for (int i = 0; i < TCARD_GRID_LINES; i++) {
        CHECK(inside(tcard_grid_rect(i, true)) && inside(tcard_grid_rect(i, false)));
    }
    tcard_rect_t b = tcard_border_rect();
    CHECK(b.x == 0 && b.y == 0 && b.w == TCARD_SIZE && b.h == TCARD_SIZE); // the outermost pixels
    tcard_rect_t c = tcard_circle_rect();
    CHECK(c.w == c.h && c.x * 2 + c.w == TCARD_SIZE && c.y * 2 + c.h == TCARD_SIZE); // round and centred
}

static void test_bars_and_wedge(void) {
    static uint32_t const want[TCARD_BARS] = {0xFF0000, 0x00FF00, 0x0000FF, 0x00FFFF,
                                              0xFF00FF, 0xFFFF00, 0xFFFFFF, 0x000000};
    for (int i = 0; i < TCARD_BARS; i++) {
        CHECK(tcard_bar_color(i) == want[i]);
        if (i) {
            CHECK(tcard_bar_rect(i).x == tcard_bar_rect(i - 1).x + tcard_bar_rect(i - 1).w); // side by side
        }
    }
    CHECK(tcard_wedge_percent(0) == 0 && tcard_wedge_percent(TCARD_WEDGE - 1) == 100);
    CHECK(tcard_wedge_color(0) == 0x000000 && tcard_wedge_color(TCARD_WEDGE - 1) == 0xFFFFFF);
    CHECK(tcard_wedge_color(5) == 0x7F7F7F); // 50 %
    for (int i = 1; i < TCARD_WEDGE; i++) {
        CHECK(tcard_wedge_percent(i) == tcard_wedge_percent(i - 1) + 10);
        CHECK(tcard_wedge_color(i) > tcard_wedge_color(i - 1));
    }
}

static void test_gratings(void) {
    int px[TCARD_GRATINGS] = {1, 2, 1, 2};
    for (int i = 0; i < TCARD_GRATINGS; i++) {
        tcard_grating_t g = tcard_grating(i);
        CHECK(g.line_px == px[i] && g.vertical == (i < 2));
        tcard_rect_t r, prev = {0};
        int          lines = 0;
        for (int k = 0; tcard_grating_line(&g, k, &r); k++, lines++) {
            CHECK(r.x >= g.area.x && r.y >= g.area.y && r.x + r.w <= g.area.x + g.area.w &&
                  r.y + r.h <= g.area.y + g.area.h);
            if (k) { // a line and an equal gap
                CHECK(g.vertical ? r.x - prev.x == 2 * g.line_px : r.y - prev.y == 2 * g.line_px);
            }
            prev = r;
        }
        CHECK(lines == 120 / (2 * g.line_px));
        CHECK(!tcard_grating_line(&g, -1, &r));
    }
}

static void test_marker(void) {
    tcard_rect_t first = tcard_marker_rect(0);
    for (uint32_t f = 1; f < TCARD_MARKER_POS; f++) { // one even step per frame
        CHECK(tcard_marker_rect(f).x - tcard_marker_rect(f - 1).x == tcard_marker_rect(1).x - first.x);
        CHECK(tcard_marker_rect(f).y == first.y);
    }
    CHECK(tcard_marker_rect(TCARD_MARKER_POS).x == first.x); // wraps
    CHECK(tcard_marker_rect(TCARD_MARKER_POS + 3).x == tcard_marker_rect(3).x);
    CHECK(tcard_marker_rect(0xFFFFFFFFu).x == tcard_marker_rect(0xFFFFFFFFu % TCARD_MARKER_POS).x);
}

static void info(tcard_info_t in, char *l1, size_t c1, char *l2, size_t c2) {
    tcard_info_text(&in, l1, c1, l2, c2);
}

static void test_info(void) {
    char l1[80], l2[64];
    info((tcard_info_t){"fw-abc1234", "468 (3.0 s ago)", "40 (1.0 s ago)", "40 (1.0 s ago)", 40, 40, "120000000 (1.0 s ago)"}, l1, sizeof(l1), l2,
         sizeof(l2));
    CHECK(strcmp(l1, "fw-abc1234   panel 46.8 Hz   psram 120 MHz") == 0);
    CHECK(strcmp(l2, "bl requested 40/40   applied 40/40") == 0);
    info((tcard_info_t){NULL, "? (not read yet)", NULL, "", 10, 0, "? (not read yet)"}, l1, sizeof(l1), l2, sizeof(l2));
    CHECK(strcmp(l1, "fw ?   panel ? Hz   psram ? MHz") == 0);
    CHECK(strcmp(l2, "bl requested 10/0   applied ?/?") == 0);
    info((tcard_info_t){"fw-abc1234", "468", "40", "0", -1, 0, "80000000"}, l1, sizeof(l1), l2, sizeof(l2)); // requested unknown
    CHECK(strcmp(l2, "bl applied 40/0") == 0);
    info((tcard_info_t){"fw-x", "500", "stale", "80", 80, 80, NULL}, l1, 8, l2, 8); // small buffers: cut, terminated
    CHECK(strlen(l1) == 7 && strlen(l2) == 7);
}

int main(void) {
    test_parse();
    test_step();
    test_geometry();
    test_bars_and_wedge();
    test_gratings();
    test_marker();
    test_info();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("test_card_layout_test: ok\n");
    return 0;
}
