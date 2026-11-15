// Host tests for the pure half of the tear-free flush (#467): which buffer is the back one, the list of areas a frame
// drew (merging, overflow that never loses a pixel), the copy-back of those areas, the rule that tells a frame-done
// callback that proves the buffer switch from one that may have raced it, and the lazy swap state machine against a
// fake scan-out (copy direction, when front flips, timeout and recovery, refusal, back-to-back and idle frames).
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

static display_frame_mark_t mark(uint32_t count, uint32_t us) {
    display_frame_mark_t const m = {count, us};
    return m;
}

static void test_swap_proven(void) {
    uint32_t const             guard = 200;
    display_frame_mark_t const req   = mark(10, 1000);
    CHECK(!display_swap_proven(req, mark(10, 900), guard));  // no callback since the request
    CHECK(!display_swap_proven(req, mark(11, 1000), guard)); // the first one, too close: may have read the old index
    CHECK(!display_swap_proven(req, mark(11, 1200), guard)); // still inside the guard
    CHECK(display_swap_proven(req, mark(11, 1201), guard));  // read the index after the request
    CHECK(display_swap_proven(req, mark(12, 1100), guard));  // a second callback proves it whatever its stamp says
    CHECK(display_swap_proven(req, mark(5000000, 1000), guard)); // a long idle: the stamp has wrapped, the count settles it
    CHECK(!display_swap_proven(req, mark(9, 99999), guard));     // a mark from before the request (torn read)
    // the 32-bit microsecond stamp wraps every 71 minutes, the count every two years
    CHECK(display_swap_proven(mark(UINT32_MAX, 0xFFFFFF00u), mark(0, 0x00000100u), guard));
    CHECK(display_swap_proven(mark(UINT32_MAX, 0), mark(1, 0), guard));
}

// Edges of the 720x720 panel, an L-shaped overlap, the same area twice
static void test_dirty_edges(void) {
    display_dirty_t d;
    display_dirty_clear(&d);
    display_dirty_add(&d, (display_rect_t){640, 0, 719, 719}); // the last panel columns (UI top strip)
    display_dirty_add(&d, (display_rect_t){0, 640, 719, 719}); // the last panel rows: an L with the first
    CHECK(d.n == 2);
    display_dirty_add(&d, (display_rect_t){640, 0, 719, 719}); // the same area again
    CHECK(d.n == 2);
    display_dirty_clear(&d);
    display_dirty_add(&d, (display_rect_t){0, 0, 9, 9});
    display_dirty_add(&d, (display_rect_t){5, 5, 14, 14}); // overlaps a corner: the box would copy 50 undrawn pixels
    CHECK(d.n == 2);
    display_dirty_add(&d, (display_rect_t){0, 0, 9, 9});
    CHECK(d.n == 2);

    static uint16_t src[720 * 720], dst[720 * 720];
    for (int i = 0; i < 720 * 720; i++) {
        src[i] = (uint16_t)(i | 1);
    }
    memset(dst, 0, sizeof(dst));
    display_copy_rect(dst, src, 720, (display_rect_t){719, 719, 719, 719}); // the very last pixel
    display_copy_rect(dst, src, 720, (display_rect_t){700, 0, 719, 3});     // touching x = 719
    CHECK(dst[720 * 720 - 1] == src[720 * 720 - 1]);
    CHECK(dst[3 * 720 + 719] == src[3 * 720 + 719] && dst[699] == 0 && dst[4 * 720 + 719] == 0);
    CHECK(dst[720 * 719 + 718] == 0);
}

// ---- the state machine, against a fake scan-out with two 8x8 buffers ----
enum { FW = 8, FH = 8 };

typedef struct {
    uint16_t buf[2][FW * FH];
    bool     refuse;   // request fails (draw_bitmap error)
    bool     prove;    // wait succeeds
    int      requests; // calls of request
    int      waits;
    unsigned requested; // buffer last requested
    int      copies;
    unsigned copy_dst[8], copy_src[8];
    uint32_t clock;
} fake_t;

static bool fake_request(void *ctx, unsigned fb, display_frame_mark_t *m) {
    fake_t *const f = ctx;
    f->requests++;
    if (f->refuse) {
        return false;
    }
    f->requested = fb;
    *m           = mark((uint32_t)f->requests, f->clock);
    return true;
}

static bool fake_wait(void *ctx, display_frame_mark_t m) {
    fake_t *const f = ctx;
    (void)m;
    f->waits++;
    f->clock += 5000;
    return f->prove;
}

static void fake_copy(void *ctx, unsigned dst, unsigned src, display_rect_t r) {
    fake_t *const f = ctx;
    if (f->copies < 8) {
        f->copy_dst[f->copies] = dst;
        f->copy_src[f->copies] = src;
    }
    f->copies++;
    f->clock += 100;
    display_copy_rect(f->buf[dst], f->buf[src], FW, r);
}

static uint32_t fake_now(void *ctx) {
    return ((fake_t *)ctx)->clock;
}

static void fake_setup(fake_t *fk, display_flip_t *f, bool swap_on) {
    memset(fk, 0, sizeof(*fk));
    fk->prove                    = true;
    display_flip_ops_t const ops = {fake_request, fake_wait, fake_copy, fake_now, fk};
    display_flip_init(f, &ops, swap_on);
}

// One LVGL refresh: every area gets the colour, written into the buffer the state machine names
static unsigned draw_frame(display_flip_t *f, fake_t *fk, display_rect_t const *areas, int n, uint16_t colour) {
    unsigned fb = 0;
    for (int i = 0; i < n; i++) {
        fb = display_flip_write_target(f);
        for (int32_t y = areas[i].y1; y <= areas[i].y2; y++) {
            for (int32_t x = areas[i].x1; x <= areas[i].x2; x++) {
                fk->buf[fb][y * FW + x] = colour;
            }
        }
        display_flip_drawn(f, areas[i]);
    }
    display_flip_frame_end(f);
    return fb;
}

static bool buffers_equal(fake_t const *fk) {
    return memcmp(fk->buf[0], fk->buf[1], sizeof(fk->buf[0])) == 0;
}

static display_rect_t const AREA_A = {0, 0, 3, 7}, AREA_B = {4, 2, 7, 5};

// Copy direction and when front flips: the request returns at once, the next write resolves it
static void test_flip_basic(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, true);
    CHECK(draw_frame(&f, &fk, &AREA_A, 1, 7) == 1); // drawn into the back buffer
    CHECK(f.pending && fk.requested == 1 && fk.waits == 0 && fk.copies == 0 && f.front == 0);
    CHECK(display_flip_write_target(&f) == 0); // proven: 1 is shown, 0 is the new back
    CHECK(f.front == 1 && !f.pending && fk.waits == 1);
    CHECK(fk.copies == 1 && fk.copy_dst[0] == 0 && fk.copy_src[0] == 1);
    CHECK(buffers_equal(&fk) && f.dirty.n == 0);
    CHECK(f.stats.swaps == 1 && f.stats.wait_us == 5000 && f.stats.copy_us == 100 && f.stats.copy_max_us == 100);
}

// Two frames back to back: each resolves the previous one before its first strip
static void test_flip_back_to_back(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, true);
    CHECK(draw_frame(&f, &fk, &AREA_A, 1, 1) == 1);
    CHECK(draw_frame(&f, &fk, &AREA_B, 1, 2) == 0); // frame A proven, copied 1 -> 0, then B drawn into 0
    CHECK(fk.requested == 0 && f.front == 1 && fk.copies == 1);
    CHECK(display_flip_write_target(&f) == 1); // B proven: copied 0 -> 1
    CHECK(f.front == 0 && fk.copies == 2 && fk.copy_dst[1] == 1 && fk.copy_src[1] == 0);
    CHECK(buffers_equal(&fk) && fk.buf[0][0] == 1 && fk.buf[0][2 * FW + 4] == 2);
}

// A frame followed by nothing: the requested buffer holds the whole picture; the other is levelled at the next write
static void test_flip_idle(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, true);
    draw_frame(&f, &fk, &AREA_A, 1, 3);
    CHECK(fk.requested == 1 && fk.buf[1][0] == 3 && fk.copies == 0 && fk.waits == 0); // nothing blocks while idle
    CHECK(!buffers_equal(&fk));
    display_flip_write_target(&f);
    CHECK(buffers_equal(&fk));
}

// A wait that runs out: no flip, no copy into a buffer that may be on screen, same back buffer again, areas kept,
// requested again at the next frame end; once proven everything drawn meanwhile reaches the other buffer
static void test_flip_timeout(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, true);
    draw_frame(&f, &fk, &AREA_A, 1, 4);
    fk.prove = false;
    CHECK(draw_frame(&f, &fk, &AREA_B, 1, 5) == 1); // still the same back buffer
    CHECK(f.front == 0 && fk.copies == 0 && f.stats.timeouts == 1 && f.dirty.n == 2);
    CHECK(f.pending && fk.requested == 1 && fk.requests == 2); // asked again
    fk.prove = true;
    CHECK(display_flip_write_target(&f) == 0);
    CHECK(f.front == 1 && fk.copies == 2 && fk.copy_dst[0] == 0 && fk.copy_dst[1] == 0);
    CHECK(buffers_equal(&fk) && fk.buf[0][0] == 4 && fk.buf[0][2 * FW + 4] == 5);
    CHECK(f.stats.swaps == 1 && f.stats.timeouts == 1);
}

// draw_bitmap refused: the frame is copied into the shown buffer at once, nothing pending
static void test_flip_refused(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, true);
    fk.refuse = true;
    draw_frame(&f, &fk, &AREA_A, 1, 6);
    CHECK(!f.pending && f.front == 0 && f.stats.refusals == 1);
    CHECK(fk.copies == 1 && fk.copy_dst[0] == 0 && fk.copy_src[0] == 1 && buffers_equal(&fk));
    CHECK(display_flip_write_target(&f) == 1 && fk.waits == 0);
}

// No swap at all (the frame callback is not ours): strips go straight into the shown buffer
static void test_flip_off(void) {
    fake_t         fk;
    display_flip_t f;
    fake_setup(&fk, &f, false);
    CHECK(draw_frame(&f, &fk, &AREA_A, 1, 8) == 0);
    CHECK(fk.requests == 0 && fk.copies == 0 && fk.waits == 0 && f.dirty.n == 0);
}

int main(void) {
    test_back_fb();
    test_full_frame_merges();
    test_out_of_order_strips();
    test_contain_disjoint_empty();
    test_overflow_covers_everything();
    test_copy_rect();
    test_swap_proven();
    test_dirty_edges();
    test_flip_basic();
    test_flip_back_to_back();
    test_flip_idle();
    test_flip_timeout();
    test_flip_refused();
    test_flip_off();
    if (failures) {
        printf("display_flip_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("display_flip_test: ok\n");
    return 0;
}
