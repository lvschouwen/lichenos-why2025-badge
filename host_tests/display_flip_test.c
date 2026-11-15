// Host tests for the pure half of the tear-free flush (#467): which buffer is the back one, the list of areas a frame
// drew (merging, overflow that never loses a pixel), the copy-back of those areas, and the rule that tells a
// frame-done callback that proves the buffer switch from one that may have raced it.
#include "display_flip.h"

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

static int same(display_rect_t a, display_rect_t b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

static void test_back_fb(void) {
    CHECK(display_back_fb(0) == 1);
    CHECK(display_back_fb(1) == 0);
    CHECK(display_back_fb(display_back_fb(0)) == 0); // a swap and a swap back
}

// A full-screen LVGL refresh: nine 720x80 UI strips, turned into nine 80x720 panel blocks, must become one area
static void test_full_frame_merges(void) {
    display_dirty_t d;
    display_dirty_clear(&d);
    for (int32_t y = 0; y < 720; y += 80) {
        display_dirty_add(&d, display_rot90_block((display_rect_t){0, y, 719, y + 79}, 720));
    }
    CHECK(d.n == 1);
    CHECK(same(d.r[0], (display_rect_t){0, 0, 719, 719}));
}

// Strips that arrive out of order still join up (the merge rescans after an area grew)
static void test_out_of_order_strips(void) {
    display_dirty_t d;
    display_dirty_clear(&d);
    display_dirty_add(&d, (display_rect_t){0, 0, 79, 719});
    display_dirty_add(&d, (display_rect_t){160, 0, 239, 719});
    CHECK(d.n == 2);
    display_dirty_add(&d, (display_rect_t){80, 0, 159, 719}); // bridges the two
    CHECK(d.n == 1);
    CHECK(same(d.r[0], (display_rect_t){0, 0, 239, 719}));
}

static void test_contain_disjoint_empty(void) {
    display_dirty_t d;
    display_dirty_clear(&d);
    display_dirty_add(&d, (display_rect_t){10, 10, 99, 99});
    display_dirty_add(&d, (display_rect_t){20, 20, 30, 30}); // inside: nothing new
    CHECK(d.n == 1 && same(d.r[0], (display_rect_t){10, 10, 99, 99}));
    display_dirty_add(&d, (display_rect_t){0, 0, 200, 200}); // around: replaces
    CHECK(d.n == 1 && same(d.r[0], (display_rect_t){0, 0, 200, 200}));
    display_dirty_add(&d, (display_rect_t){300, 300, 310, 310}); // far away: its own area, no wasted copy
    CHECK(d.n == 2);
    display_dirty_add(&d, (display_rect_t){5, 5, 4, 9}); // empty: ignored
    CHECK(d.n == 2);
    // diagonal neighbours do not tile: merging them would copy pixels nobody drew
    display_dirty_clear(&d);
    display_dirty_add(&d, (display_rect_t){0, 0, 9, 9});
    display_dirty_add(&d, (display_rect_t){10, 10, 19, 19});
    CHECK(d.n == 2);
}

// More scattered areas than the list holds: the list stays bounded and still covers every pixel drawn
static void test_overflow_covers_everything(void) {
    enum { N = 64 };
    static unsigned char drawn[N][N];
    display_dirty_t      d;
    display_dirty_clear(&d);
    memset(drawn, 0, sizeof(drawn));
    for (int32_t i = 0; i < 40; i++) {
        int32_t const x = (i * 13) % 60, y = (i * 29) % 60; // scattered 2x2 dots, none touching in a tiling way
        display_rect_t const r = {x, y, x + 1, y + 1};
        display_dirty_add(&d, r);
        for (int32_t py = r.y1; py <= r.y2; py++) {
            for (int32_t px = r.x1; px <= r.x2; px++) {
                drawn[py][px] = 1;
            }
        }
        CHECK(d.n <= DISPLAY_DIRTY_MAX);
    }
    CHECK(d.n == DISPLAY_DIRTY_MAX);
    for (int32_t py = 0; py < N; py++) {
        for (int32_t px = 0; px < N; px++) {
            if (!drawn[py][px]) {
                continue;
            }
            int covered = 0;
            for (uint32_t k = 0; k < d.n; k++) {
                covered |= px >= d.r[k].x1 && px <= d.r[k].x2 && py >= d.r[k].y1 && py <= d.r[k].y2;
            }
            CHECK(covered);
        }
    }
}

// The copy-back touches exactly the rectangle, rows at the frame buffer stride
static void test_copy_rect(void) {
    enum { W = 12, H = 10 };
    uint16_t src[W * H], dst[W * H];
    for (int i = 0; i < W * H; i++) {
        src[i] = (uint16_t)(0x8000 | i);
        dst[i] = 0;
    }
    display_rect_t const r = {3, 2, 7, 5};
    display_copy_rect(dst, src, W, r);
    for (int32_t y = 0; y < H; y++) {
        for (int32_t x = 0; x < W; x++) {
            int const inside = x >= r.x1 && x <= r.x2 && y >= r.y1 && y <= r.y2;
            CHECK(dst[y * W + x] == (inside ? src[y * W + x] : 0));
        }
    }
}

static void test_swap_proven(void) {
    uint32_t const guard = 200;
    CHECK(!display_swap_proven(1000, 900, guard));  // stamped before the request: an old frame
    CHECK(!display_swap_proven(1000, 1000, guard)); // could have read the old index
    CHECK(!display_swap_proven(1000, 1200, guard)); // still inside the guard
    CHECK(display_swap_proven(1000, 1201, guard));  // the DMA read the index after the request
    CHECK(display_swap_proven(1000, 22000, guard)); // a whole frame later
    // the 32-bit microsecond stamp wraps every 71 minutes
    CHECK(display_swap_proven(0xFFFFFF00u, 0x00000100u, guard));
    CHECK(!display_swap_proven(0x00000100u, 0xFFFFFF00u, guard));
}

int main(void) {
    test_back_fb();
    test_full_frame_merges();
    test_out_of_order_strips();
    test_contain_disjoint_empty();
    test_overflow_covers_everything();
    test_copy_rect();
    test_swap_proven();
    if (failures) {
        printf("display_flip_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("display_flip_test: ok\n");
    return 0;
}
