// Host test: the station clock screensaver's hands and geometry (#119)
#include "station_clock.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static int inside(sc_box_t b, int32_t x, int32_t y) {
    return x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2;
}

static int32_t dist2(sc_point_t p, int32_t cx, int32_t cy) {
    return (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy);
}

static void test_schedule(void) {
    // The sweep: 0 at the start of the minute, a quarter at a quarter of the sweep, never back while it sweeps
    CHECK(sc_second_angle(0) == 0);
    CHECK(sc_second_angle(SC_SWEEP_MS / 4) == 900);
    CHECK(sc_second_angle(SC_SWEEP_MS / 2) == 1800);
    CHECK(sc_second_angle(SC_SWEEP_MS - 1) == 3599);
    int32_t prev = -1;
    for (uint32_t ms = 0; ms < SC_SWEEP_MS; ms += 50) {
        int32_t a = sc_second_angle(ms);
        CHECK(a >= prev && a >= 0 && a < 3600);
        prev = a;
    }
    // A whole turn in 58.5 s: faster than the real seconds (30 s is already past half way)
    CHECK(sc_second_angle(30000) > 1800);
    // The pause: at 12 from 58.5 s to the minute (and a leap second), about 1.5 s
    CHECK(SC_MINUTE_MS - SC_SWEEP_MS >= 1000 && SC_MINUTE_MS - SC_SWEEP_MS <= 2000);
    for (uint32_t ms = SC_SWEEP_MS; ms < SC_MINUTE_MS + 1000; ms += 100) {
        CHECK(sc_second_angle(ms) == 0);
    }

    // The minute hand jumps on the minute and stands still between
    CHECK(sc_minute_angle(0) == 0);
    CHECK(sc_minute_angle(15) == 900);
    CHECK(sc_minute_angle(59) == 3540);
    CHECK(sc_minute_angle(60) == 0);
    // The hour hand from hours and minutes: half a degree a minute, 12-hour dial
    CHECK(sc_hour_angle(0, 0) == 0);
    CHECK(sc_hour_angle(12, 0) == 0);
    CHECK(sc_hour_angle(3, 0) == 900);
    CHECK(sc_hour_angle(15, 30) == 1050);
    CHECK(sc_hour_angle(23, 59) == 3595);
    CHECK(sc_hour_angle(9, 1) - sc_hour_angle(9, 0) == 5);
}

static void test_polar(void) {
    sc_point_t p = sc_polar(100, 100, 0, 50);
    CHECK(p.x == 100 && p.y == 50); // 12 is up
    p = sc_polar(100, 100, 900, 50);
    CHECK(p.x == 150 && p.y == 100); // 3 o'clock is right: clockwise
    p = sc_polar(100, 100, 1800, 50);
    CHECK(p.x == 100 && p.y == 150);
    p = sc_polar(100, 100, 2700, 50);
    CHECK(p.x == 50 && p.y == 100);
    p = sc_polar(100, 100, 0, -20); // a tail points the other way
    CHECK(p.x == 100 && p.y == 120);

    sc_point_t a, b;
    sc_bar_ends(300, 300, 900, (sc_bar_t){-10, 200, 8}, &a, &b);
    CHECK(a.x == 290 && a.y == 300 && b.x == 500 && b.y == 300);
}

static void test_dims(void) {
    int32_t const r = 288, c = 288;
    sc_dims_t     d = sc_dims(r);
    // Marks inside the dial; hour marks longer and thicker than minute marks; both end on the same circle
    CHECK(d.hour_mark.r1 < r && d.minute_mark.r1 == d.hour_mark.r1);
    CHECK(d.hour_mark.r0 < d.minute_mark.r0 && d.hour_mark.w > 2 * d.minute_mark.w);
    // Hands: the hour hand broad and short, the minute hand slimmer and long, the second hand thin; all inside
    CHECK(d.hour.w > d.minute.w && d.minute.w > 3 * d.second.w && d.second.w >= 2);
    CHECK(d.hour.r1 < d.minute.r1 && d.minute.r1 < r && d.minute.r1 > d.minute_mark.r0);
    CHECK(d.disc_r > d.second.w && d.second.r1 + d.disc_r < d.hour_mark.r1);
    CHECK(d.cap_r > d.second.w / 2);
    CHECK(d.second.r0 < 0 && d.hour.r0 < 0 && d.minute.r0 < 0);
    // Every hand's box stays inside the dial's square at every angle
    for (int32_t a = 0; a < 3600; a += 25) {
        sc_bar_t const bars[] = {d.hour, d.minute, d.second, d.hour_mark, d.minute_mark};
        for (unsigned i = 0; i < sizeof(bars) / sizeof(bars[0]); i++) {
            sc_box_t bx = sc_bar_box(c, c, a, bars[i].w, bars[i].r0, bars[i].r1, 2);
            CHECK(bx.x1 >= 0 && bx.y1 >= 0 && bx.x2 < 2 * r && bx.y2 < 2 * r);
        }
        sc_box_t db = sc_disc_box(c, c, a, d.second.r1, d.disc_r, 2);
        CHECK(db.x1 >= 0 && db.y1 >= 0 && db.x2 < 2 * r && db.y2 < 2 * r);
        // Ends of the marks stay inside the dial circle
        sc_point_t p0, p1;
        sc_bar_ends(c, c, a, d.minute_mark, &p0, &p1);
        CHECK(dist2(p1, c, c) < r * r);
    }
    CHECK(sc_dims(40).minute_mark.w >= 2 && sc_dims(40).second.w >= 2);
}

static void test_boxes(void) {
    int32_t const cx = 300, cy = 300;
    // Upright: exactly the bar's rectangle, grown by the pad
    sc_box_t      b = sc_bar_box(cx, cy, 0, 10, -20, 100, 2);
    CHECK(b.x1 == 293 && b.x2 == 307 && b.y1 == 198 && b.y2 == 322);
    // At 3 o'clock the same, turned
    b = sc_bar_box(cx, cy, 900, 10, -20, 100, 0);
    CHECK(b.x1 == 280 && b.x2 == 400 && b.y1 == 295 && b.y2 == 305);
    // Every pixel the bar covers is in its box, at every angle; and the box is not much bigger than the bar's own
    // extent
    for (int32_t a = 0; a < 3600; a += 7) {
        int32_t const w = 12, r0 = -40, r1 = 250;
        b = sc_bar_box(cx, cy, a, w, r0, r1, 1);
        for (int32_t r = r0; r <= r1; r += 5) {
            for (int32_t off = -w / 2; off <= w / 2; off += 2) {
                sc_point_t m = sc_polar(cx, cy, a, r);
                sc_point_t q = sc_polar(m.x, m.y, a + 900, off); // across the bar
                CHECK(inside(b, q.x, q.y));
            }
        }
        CHECK(b.x2 - b.x1 <= 290 + w + 4 && b.y2 - b.y1 <= 290 + w + 4);
        // Pieces cover the whole bar, and a diagonal hand in pieces is far less area than its one box
        int64_t  pieces = 0;
        int32_t  n      = 4;
        sc_box_t whole  = b;
        for (int32_t k = 0; k < n; k++) {
            sc_box_t p = sc_bar_box(cx, cy, a, w, r0 + (r1 - r0) * k / n, r0 + (r1 - r0) * (k + 1) / n, 1);
            CHECK(p.x1 >= whole.x1 && p.x2 <= whole.x2 && p.y1 >= whole.y1 && p.y2 <= whole.y2);
            pieces += (int64_t)(p.x2 - p.x1 + 1) * (p.y2 - p.y1 + 1);
        }
        if (a % 900 > 300 && a % 900 < 600) {
            CHECK(pieces * 2 < (int64_t)(whole.x2 - whole.x1 + 1) * (whole.y2 - whole.y1 + 1));
        }
    }
    // The disc's box: centred on the point at that radius
    sc_box_t d = sc_disc_box(cx, cy, 900, 100, 20, 1);
    CHECK(d.x1 == 379 && d.x2 == 421 && d.y1 == 279 && d.y2 == 321);
}

int main(void) {
    test_schedule();
    test_polar();
    test_dims();
    test_boxes();
    if (failures) {
        printf("station_clock_test: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("station_clock_test: OK\n");
    return EXIT_SUCCESS;
}
