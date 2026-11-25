// Host tests for the video surface geometry (#559): 1/16 scale steps, the centred output rect, its panel-frame
// block (checked pixel by pixel against the rotation LVGL/the PPA use), the bars, strip hit test and split.
#include "display_video_geom.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static bool req(display_rect_t r, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    return r.x1 == x1 && r.y1 == y1 && r.x2 == x2 && r.y2 == y2;
}

static int32_t area(display_rect_t r) {
    return (r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
}

static void test_scale(void) {
    CHECK(display_video_scale16(2250) == 36);
    CHECK(display_video_scale16(2700) == 43); // 43.2 -> 43 = 2.6875
    CHECK(display_video_scale16(1000) == 16);
    CHECK(display_video_scale16(0) == 0);
    CHECK(display_video_scale16(30) == 0);  // 0.48 steps
    CHECK(display_video_scale16(40) == 1);  // 0.64 steps
    CHECK(display_video_scale16(4294967295u) > 0);
}

static void test_doom(void) {
    display_video_geom_t g;
    CHECK(display_video_geom_make(&g, 320, 200, 720, 720, 2250, 2700));
    CHECK(g.scale_x16 == 36 && g.scale_y16 == 43);
    CHECK(req(g.rect, 0, 91, 719, 627)); // 720 x 537
    CHECK(g.bar_ok[0] && g.bar_ok[1]);
    CHECK(req(g.bar[0], 0, 0, 719, 90));    // 91 rows
    CHECK(req(g.bar[1], 0, 628, 719, 719)); // 92 rows
    // Panel frame: UI (x, y) -> panel (y, 719 - x)
    CHECK(req(g.rect_blk, 91, 0, 627, 719));
    CHECK(req(g.bar_blk[0], 0, 0, 90, 719));
    CHECK(req(g.bar_blk[1], 628, 0, 719, 719));
    CHECK(area(g.rect) + area(g.bar[0]) + area(g.bar[1]) == 720 * 720);
    CHECK(area(g.rect_blk) == area(g.rect));
}

// Every pixel of the rect and the bars lands in its block, and the three blocks tile the panel exactly once
static void test_tiling(void) {
    display_video_geom_t g;
    CHECK(display_video_geom_make(&g, 320, 200, 720, 720, 2250, 2700));
    static unsigned char hit[720][720];
    memset(hit, 0, sizeof hit);
    display_rect_t const bl[3] = {g.rect_blk, g.bar_blk[0], g.bar_blk[1]};
    for (int b = 0; b < 3; b++)
        for (int32_t y = bl[b].y1; y <= bl[b].y2; y++)
            for (int32_t x = bl[b].x1; x <= bl[b].x2; x++)
                hit[y][x]++;
    int bad = 0;
    for (int y = 0; y < 720; y++)
        for (int x = 0; x < 720; x++)
            bad += hit[y][x] != 1;
    CHECK(bad == 0);
    // A UI pixel inside the video lands inside rect_blk, per the rotation (x, y) -> (y, H - 1 - x)
    for (int32_t y = g.rect.y1; y <= g.rect.y2; y += 37)
        for (int32_t x = g.rect.x1; x <= g.rect.x2; x += 41) {
            int32_t const px = y, py = 719 - x;
            CHECK(px >= g.rect_blk.x1 && px <= g.rect_blk.x2 && py >= g.rect_blk.y1 && py <= g.rect_blk.y2);
        }
}

static void test_odd_and_edges(void) {
    display_video_geom_t g;
    // Odd leftover: 7 px of UI height, 4 of video -> 1 above, 2 below (the odd pixel goes below)
    CHECK(display_video_geom_make(&g, 4, 4, 7, 7, 1000, 1000));
    CHECK(req(g.rect, 1, 1, 4, 4)); // (7-4)/2 = 1 on each axis
    CHECK(req(g.bar[0], 0, 0, 6, 0));
    CHECK(req(g.bar[1], 0, 5, 6, 6));
    CHECK(!display_video_geom_make(&g, 4, 4, 10, 7, 1000, 1000)); // not square
    // Exactly fills the UI: no bars
    CHECK(display_video_geom_make(&g, 45, 45, 720, 720, 16000, 16000));
    CHECK(req(g.rect, 0, 0, 719, 719) && !g.bar_ok[0] && !g.bar_ok[1]);
    // One bar only
    CHECK(display_video_geom_make(&g, 10, 9, 10, 10, 1000, 1000));
    CHECK(!g.bar_ok[0] && g.bar_ok[1] && req(g.bar[1], 0, 9, 9, 9));
    // Refused: zero sizes or scales, too big, scale rounds to nothing
    CHECK(!display_video_geom_make(&g, 0, 200, 720, 720, 2250, 2700));
    CHECK(!display_video_geom_make(&g, 320, 0, 720, 720, 2250, 2700));
    CHECK(!display_video_geom_make(&g, 320, 200, 720, 720, 0, 2700));
    CHECK(!display_video_geom_make(&g, 320, 200, 720, 720, 2250, 0));
    CHECK(!display_video_geom_make(&g, 320, 200, 0, 720, 2250, 2700));
    CHECK(!display_video_geom_make(&g, 320, 200, 720, 720, 2300, 2700)); // 36.8 -> 37: 740 wide
    CHECK(!display_video_geom_make(&g, 320, 200, 720, 720, 2250, 3700)); // 59 -> 737 high
    CHECK(!display_video_geom_make(&g, 1, 1, 720, 720, 30, 30));
    CHECK(!display_video_geom_make(&g, 70000, 1, 720, 720, 1000, 1000));
}

static void test_hit(void) {
    display_video_geom_t g;
    CHECK(display_video_geom_make(&g, 320, 200, 720, 720, 2250, 2700));
    CHECK(!display_video_hits(&g, (display_rect_t){0, 0, 719, 55}));    // status bar
    CHECK(!display_video_hits(&g, (display_rect_t){0, 0, 719, 90}));    // the whole top bar
    CHECK(display_video_hits(&g, (display_rect_t){0, 0, 719, 91}));     // one row in
    CHECK(display_video_hits(&g, (display_rect_t){100, 300, 200, 350})); // fully inside
    CHECK(display_video_hits(&g, (display_rect_t){0, 627, 0, 627}));    // last row, one pixel
    CHECK(!display_video_hits(&g, (display_rect_t){0, 628, 719, 719}));
    CHECK(display_video_hits(&g, (display_rect_t){0, 0, 719, 719}));    // everything
    CHECK(!display_video_hits(&g, (display_rect_t){5, 5, 4, 4}));       // empty
}

static void test_split(void) {
    display_video_geom_t g;
    display_rect_t       o[DISPLAY_VIDEO_SPLIT_MAX];
    CHECK(display_video_geom_make(&g, 320, 200, 720, 720, 2250, 2700));
    // Outside: the strip itself
    CHECK(display_video_split(&g, (display_rect_t){0, 0, 719, 55}, o) == 1 && req(o[0], 0, 0, 719, 55));
    // Fully inside: nothing
    CHECK(display_video_split(&g, (display_rect_t){10, 100, 20, 200}, o) == 0);
    // Straddling the top edge: only the band above
    CHECK(display_video_split(&g, (display_rect_t){0, 60, 719, 139}, o) == 1 && req(o[0], 0, 60, 719, 90));
    // Straddling both edges: above and below
    CHECK(display_video_split(&g, (display_rect_t){0, 0, 719, 719}, o) == 2);
    CHECK(req(o[0], 0, 0, 719, 90) && req(o[1], 0, 628, 719, 719));
    // Empty strip
    CHECK(display_video_split(&g, (display_rect_t){3, 3, 2, 2}, o) == 0);

    // A video narrower than the UI: four parts that do not overlap and cover strip minus video exactly
    display_video_geom_t n;
    CHECK(display_video_geom_make(&n, 4, 4, 20, 20, 1000, 1000)); // rect 8..11 x 8..11
    display_rect_t const s = {2, 5, 17, 14};
    CHECK(display_video_split(&n, s, o) == 4);
    int32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += area(o[i]);
        CHECK(!display_video_hits(&n, o[i]));
        CHECK(o[i].x1 >= s.x1 && o[i].x2 <= s.x2 && o[i].y1 >= s.y1 && o[i].y2 <= s.y2);
        for (int j = i + 1; j < 4; j++)
            CHECK(o[i].x2 < o[j].x1 || o[j].x2 < o[i].x1 || o[i].y2 < o[j].y1 || o[j].y2 < o[i].y1);
    }
    CHECK(sum == area(s) - 16);
    // Straddling one side only
    CHECK(display_video_split(&n, (display_rect_t){5, 9, 9, 10}, o) == 1 && req(o[0], 5, 9, 7, 10));
}

int main(void) {
    test_scale();
    test_doom();
    test_tiling();
    test_odd_and_edges();
    test_hit();
    test_split();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("display_video_geom_test: ok\n");
    return 0;
}
