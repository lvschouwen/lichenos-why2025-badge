// lichen_video.h's decisions (#562, lichen_video_rules.c): an app without VIDEO cannot acquire or draw, the frame cycle
// alternates only on a shown frame, a record the owner no longer holds hands out nothing, and release is idempotent.
#include "lichen_video_rules.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

static struct {
    bool     cap, holds, present_ok, release_ok;
    display_video_refusal_t refusal;
    unsigned acquires, presents, last_idx, releases;
    uint32_t last_w, last_h, last_sx, last_sy, last_ctx;
    char     a[16], b[16];
} f;

static bool     cap_ok(void) { return f.cap; }
static uint32_t self(void) { return 0x1234u; }
static display_video_refusal_t acquire(uint32_t ctx, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy,
                                       void (*cb)(void *, display_video_reclaim_t), void *arg, void *src[2]) {
    (void)cb; (void)arg;
    f.acquires++; f.last_ctx = ctx; f.last_w = w; f.last_h = h; f.last_sx = sx; f.last_sy = sy;
    if (f.refusal == DISPLAY_VIDEO_REFUSE_NONE) {
        src[0] = f.a; src[1] = f.b; f.holds = true;
    }
    return f.refusal;
}
static bool present(uint32_t ctx, unsigned idx) { (void)ctx; f.presents++; f.last_idx = idx; return f.present_ok; }
static bool release(uint32_t ctx) { (void)ctx; f.releases++; if (f.release_ok) { f.holds = false; } return f.release_ok; }
static bool holds(uint32_t ctx) { return f.holds && ctx == 0x1234u; }

static lichen_video_ops_t const OPS = {cap_ok, self, acquire, present, release, holds};

static unsigned reclaimed_why;
static void on_reclaim(void *arg, lichen_video_reclaim_t why) { (void)arg; reclaimed_why = (unsigned)why; }
static void owner_cb(void *arg, display_video_reclaim_t why) { (void)arg; (void)why; }

static lichen_video_cfg_t const CFG = {.src_w = 320, .src_h = 200, .on_reclaim = on_reclaim};

static void reset(void) {
    memset(&f, 0, sizeof(f));
    f.cap = f.present_ok = f.release_ok = true;
    reclaimed_why = 0;
}

static void test_refused_without_cap(void) {
    reset();
    lichen_video_t v = {0};
    f.cap = false;
    CHECK(!lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    CHECK(f.acquires == 0); // the owner is never reached
    f.cap = true; // an app that did acquire, then lost the cap (cannot happen) or another app: no pointer, no frame
    CHECK(lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    f.cap = false;
    CHECK(lichen_video_do_back(&v, &OPS) == NULL);
    CHECK(!lichen_video_do_present(&v, &OPS) && f.presents == 0);
    CHECK(!lichen_video_do_release(&v, &OPS) && f.releases == 0);
}

static void test_acquire(void) {
    reset();
    lichen_video_t v = {0};
    CHECK(!lichen_video_do_acquire(&v, &OPS, NULL, owner_cb));
    lichen_video_cfg_t zero = CFG;
    zero.src_h = 0;
    CHECK(!lichen_video_do_acquire(&v, &OPS, &zero, owner_cb) && f.acquires == 0);
    f.refusal = DISPLAY_VIDEO_REFUSE_OWNED;
    CHECK(!lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb) && v.src[0] == NULL);
    f.refusal = DISPLAY_VIDEO_REFUSE_NONE;
    CHECK(lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    CHECK(f.last_ctx == 0x1234u && f.last_w == 320 && f.last_h == 200);
    CHECK(f.last_sx == LICHEN_VIDEO_SCALE_X_MILLI && f.last_sy == LICHEN_VIDEO_SCALE_Y_MILLI);
}

static void test_cycle(void) {
    reset();
    lichen_video_t v = {0};
    CHECK(lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    CHECK((char *)lichen_video_do_back(&v, &OPS) == f.b); // the first frame goes into source 1
    CHECK(lichen_video_do_present(&v, &OPS) && f.last_idx == 1);
    CHECK((char *)lichen_video_do_back(&v, &OPS) == f.a);
    f.present_ok = false; // dropped: the same buffer again
    CHECK(!lichen_video_do_present(&v, &OPS) && f.last_idx == 0);
    CHECK((char *)lichen_video_do_back(&v, &OPS) == f.a);
    f.present_ok = true;
    CHECK(lichen_video_do_present(&v, &OPS) && f.last_idx == 0);
    CHECK((char *)lichen_video_do_back(&v, &OPS) == f.b);
}

static void test_stale_and_release(void) {
    reset();
    lichen_video_t v = {0};
    CHECK(lichen_video_do_release(&v, &OPS) && f.releases == 0); // nothing held
    CHECK(lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    f.holds = false; // the unload sweep freed the sources: no pointer to them may leave
    CHECK(lichen_video_do_back(&v, &OPS) == NULL);
    CHECK(!lichen_video_do_present(&v, &OPS) && f.presents == 0);
    f.holds = true;
    f.release_ok = false; // lock timeout: kept, retry
    CHECK(!lichen_video_do_release(&v, &OPS) && lichen_video_do_back(&v, &OPS));
    f.release_ok = true;
    CHECK(lichen_video_do_release(&v, &OPS) && lichen_video_do_back(&v, &OPS) == NULL);
    CHECK(lichen_video_do_release(&v, &OPS));
}

static void test_reclaim_forward(void) {
    reset();
    lichen_video_t v = {0};
    CHECK(lichen_video_do_acquire(&v, &OPS, &CFG, owner_cb));
    lichen_video_do_reclaimed(&v, DISPLAY_VIDEO_RECLAIM_ESCAPE);
    CHECK(reclaimed_why == LICHEN_VIDEO_RECLAIM_ESCAPE);
    reclaimed_why = 0;
    lichen_video_do_reclaimed(&v, DISPLAY_VIDEO_RECLAIM_NONE);
    lichen_video_do_reclaimed(&v, DISPLAY_VIDEO_RECLAIM_COUNT);
    CHECK(reclaimed_why == 0);
}

int main(void) {
    test_refused_without_cap();
    test_acquire();
    test_cycle();
    test_stale_and_release();
    test_reclaim_forward();
    if (failures) {
        printf("lichen_video_rules_test: %d failed\n", failures);
        return 1;
    }
    printf("lichen_video_rules_test: ok\n");
    return 0;
}
