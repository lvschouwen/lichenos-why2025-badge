// Host test: the clock screensaver's digits and drift (#75)
#include "saver_layout.h"

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

static int bits(uint8_t m) {
    int n = 0;
    for (; m; m >>= 1) {
        n += m & 1;
    }
    return n;
}

static int overlap(seg_rect_t a, seg_rect_t b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

int main(void) {
    // Segment counts of the digits, the dash, nothing else
    int const lit[10] = {6, 2, 5, 5, 4, 5, 6, 3, 7, 6};
    for (int d = 0; d < 10; d++) {
        CHECK(bits(seg7_mask((char)('0' + d))) == lit[d]);
    }
    CHECK(seg7_mask('8') == 0x7F);
    CHECK(seg7_mask('1') == 0x06); // b and c: the right-hand bars
    CHECK(seg7_mask('-') == 0x40); // g only
    CHECK(seg7_mask(':') == 0 && seg7_mask(' ') == 0);

    // Segments stay inside the box, have size, and do not overlap
    int32_t const w = 110, h = 200, t = 22;
    for (int s = 0; s < SEG7_COUNT; s++) {
        seg_rect_t r = seg7_rect(s, w, h, t);
        CHECK(r.w > 0 && r.h > 0);
        CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h);
        for (int o = 0; o < s; o++) {
            CHECK(!overlap(r, seg7_rect(o, w, h, t)));
        }
    }
    CHECK(seg7_rect(0, w, h, t).y == 0 && seg7_rect(3, w, h, t).y + t == h); // a on top, d at the bottom
    CHECK(seg7_rect(9, w, h, t).w == 0);

    // Drift: centred at first, inside the spans, never the same place two minutes running, reaches the edges
    int32_t x, y, px, py, min_x = 0, max_x = 0, min_y = 0, max_y = 0;
    saver_drift(0, 80, 140, &x, &y);
    CHECK(x == 0 && y == 0);
    px = x;
    py = y;
    for (uint32_t step = 1; step < 2000; step++) {
        saver_drift(step, 80, 140, &x, &y);
        CHECK(abs(x) <= 80 && abs(y) <= 140);
        CHECK(x != px || y != py);
        CHECK(abs(x - px) <= 23 && abs(y - py) <= 17); // slight moves only
        min_x = x < min_x ? x : min_x;
        max_x = x > max_x ? x : max_x;
        min_y = y < min_y ? y : min_y;
        max_y = y > max_y ? y : max_y;
        px = x;
        py = y;
    }
    CHECK(min_x <= -70 && max_x >= 70 && min_y <= -130 && max_y >= 130);
    saver_drift(5, 0, 0, &x, &y); // no room: stays put
    CHECK(x == 0 && y == 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("saver_layout_test ok\n");
    return 0;
}
