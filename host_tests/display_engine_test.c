// Host tests for the runtime rotation engine of the direct flush (#468): names and validation, select at and away from
// a frame boundary, and the order of strips, PPA transfers, the copy-back and the swap request against a fake PPA and
// a fake scan-out. The fake counts every CPU write into a frame buffer, copy-back and swap request made while a PPA
// transfer is still running: the background engine must make none. Timeouts and refused starts fall back to the CPU
// and redo the strip, the stats line gets a rotate time per strip.
#include "display_engine.h"

#include <stdio.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define LOG_MAX 128

typedef struct {
    char     log[LOG_MAX]; // C cpu turn, S PPA start, W wait taken, T wait timed out, K copy-back, R swap request
    int      n;
    bool     hw_busy;      // the fake PPA is writing a frame buffer
    int      overlaps;     // CPU write, copy-back or request while hw_busy
    bool     refuse_start; // the driver says no
    bool     never_done;   // the transfer hangs
    bool     late_done;    // the transfer finishes only inside the late wait (after the first bound)
    bool     strict;       // fell_back does not stop the fake PPA: only its done does
    bool     ready_in_isr; // what the last start asked for
    uint32_t now;
    uint32_t rotate_us;                         // how long a fake transfer takes
    uint32_t frames;                            // the scan-out's end-of-frame count
    int      fell_back[DISPLAY_FALLBACK_COUNT]; // by reason
    int      cpu_fb_last;                       // buffer of the last CPU turn
    display_strip_t cpu_last;
    uint32_t        gen_last; // tag of the last start
    int64_t         copied_px; // pixels the copy-backs moved
} fake_t;

static void note(fake_t *fk, char c) {
    if (fk->n < LOG_MAX - 1) {
        fk->log[fk->n++] = c;
        fk->log[fk->n]   = '\0';
    }
}

static void check_idle(fake_t *fk) {
    if (fk->hw_busy) {
        fk->overlaps++;
    }
}

static void fake_cpu(void *ctx, display_strip_t const *s, unsigned fb) {
    fake_t *fk = ctx;
    check_idle(fk);
    note(fk, 'C');
    fk->cpu_fb_last  = (int)fb;
    fk->cpu_last     = *s;
    fk->now         += 3000;
}

static bool fake_start(void *ctx, display_strip_t const *s, unsigned fb, bool ready_in_isr, uint32_t gen) {
    fake_t *fk   = ctx;
    fk->gen_last = gen;
    (void)s;
    (void)fb;
    if (fk->refuse_start) {
        return false;
    }
    check_idle(fk); // one transfer at a time (max_pending_trans_num 1)
    note(fk, 'S');
    fk->hw_busy      = true;
    fk->ready_in_isr = ready_in_isr;
    return true;
}

static bool fake_wait(void *ctx, uint32_t *done_us, bool late) {
    fake_t *fk = ctx;
    if (fk->late_done && late) {
        note(fk, 'L');
        fk->hw_busy = false;
        *done_us    = fk->now;
        return true;
    }
    if (fk->never_done || fk->late_done) {
        note(fk, 'T');
        return false; // hw_busy stays until the engine gives up on it (fake_fell_back)
    }
    note(fk, 'W');
    fk->hw_busy  = false;
    *done_us     = fk->now + fk->rotate_us;
    fk->now     += fk->rotate_us;
    return true;
}

static void fake_fell_back(void *ctx, display_engine_id_t from, display_fallback_t why) {
    fake_t *fk = ctx;
    (void)from;
    fk->fell_back[why]++;
    if (!fk->strict) {
        fk->hw_busy = false; // a hung transfer: the fake stops counting its window as busy
    }
}

static uint32_t fake_now(void *ctx) {
    return ((fake_t *)ctx)->now;
}

// The scan-out: requests always taken, every wait proven (display_flip_test.c covers its races)
static bool flip_request(void *ctx, unsigned fb) {
    fake_t *fk = ctx;
    (void)fb;
    check_idle(fk);
    note(fk, 'R');
    return true;
}

static uint32_t flip_count(void *ctx) {
    return ((fake_t *)ctx)->frames;
}

static bool flip_wait(void *ctx, display_frame_mark_t m) {
    fake_t *fk = ctx;
    (void)m;
    fk->frames += 2;
    return true;
}

static void flip_copy(void *ctx, unsigned dst, unsigned src, display_rect_t r) {
    fake_t *fk = ctx;
    (void)dst;
    (void)src;
    fk->copied_px += (int64_t)(r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
    check_idle(fk);
    note(fk, 'K');
}

static uint32_t flip_now(void *ctx) {
    return ((fake_t *)ctx)->now;
}

static void setup(fake_t *fk, display_engine_t *e, display_flip_t *f) {
    memset(fk, 0, sizeof(*fk));
    fk->rotate_us                   = 1500;
    fk->cpu_fb_last                 = -1;
    display_engine_ops_t const eops = {fake_cpu, fake_start, fake_wait, fake_fell_back, fake_now, fk};
    display_flip_ops_t const   fops = {flip_request, flip_count, flip_wait, flip_copy, flip_now, fk};
    display_engine_init(e, &eops);
    display_flip_init(f, &fops, true);
}

static display_strip_t strip(int i) {
    display_strip_t const s =
        {.ui = {0, i * 80, 719, i * 80 + 79}, .blk = {i * 80, 0, i * 80 + 79, 719}, .px = NULL, .stride = 1440};
    return s;
}

#define STRIP(i) {{0, (i) * 80, 719, (i) * 80 + 79}, {(i) * 80, 0, (i) * 80 + 79, 719}, NULL, 1440, 0, 0, 0, {0, 0, 0, 0}}
static display_strip_t const S0 = STRIP(0), S1 = STRIP(1), S4 = STRIP(4);

// One refresh of n strips as LVGL runs it: wait_for_flushing (flush_wait_cb = settle) before every strip but the
// first while the previous one is still flushing, then the flush callback. Returns how many strips the callback
// readied itself.
static int refresh(display_engine_t *e, display_flip_t *f, int n) {
    int  ready    = 0;
    bool flushing = false;
    for (int i = 0; i < n; i++) {
        if (flushing) {
            display_engine_settle(e);
        }
        display_strip_t const s    = strip(i);
        bool const            now  = display_engine_flush(e, f, &s, i == n - 1);
        ready                     += now;
        flushing                   = !now;
    }
    return ready;
}

static void test_names(void) {
    CHECK(strcmp(display_engine_name(DISPLAY_ENGINE_CPU), "cpu") == 0);
    CHECK(strcmp(display_engine_name(DISPLAY_ENGINE_PPA_ASYNC), "ppa-async") == 0);
    CHECK(strcmp(display_engine_name(2), "?") == 0);
    CHECK(display_engine_valid(1) && !display_engine_valid(2) && !display_engine_valid(UINT32_MAX));
}

static void test_cpu_default(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
    CHECK(refresh(&e, &f, 3) == 3);
    CHECK(strcmp(fk.log, "CCCR") == 0);
    CHECK(e.stats.strips == 3 && e.stats.rotate_us == 9000 && e.stats.rotate_max_us == 3000);
}

static void test_select(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    CHECK(!display_engine_select(&e, 2));
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
    refresh(&e, &f, 2);
    CHECK(e.stats.strips == 2);
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC)); // at a boundary: at once, stats cleared
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC && e.stats.strips == 0);
    // mid-frame (not possible under the LVGL lock, but never mid-refresh): waits for the next refresh
    display_engine_flush(&e, &f, &S0, false);
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_CPU));
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC);
    display_engine_flush(&e, &f, &S1, true);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC); // the rest of the refresh keeps its engine
    fk.n = 0;
    refresh(&e, &f, 1);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
    CHECK(fk.log[fk.n - 2] == 'C');
}

// The background PPA: strips 0..n-2 are readied by the interrupt, the last one waits before the swap request, and the
// next refresh's copy-back comes after the last transfer
static void test_ppa_async_order(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    CHECK(refresh(&e, &f, 3) == 1); // only the last strip readied by the callback
    CHECK(strcmp(fk.log, "SWSWSWR") == 0);
    CHECK(!fk.hw_busy); // the refresh ended with nothing in flight
    CHECK(refresh(&e, &f, 3) == 1);
    CHECK(strcmp(fk.log, "SWSWSWRKSWSWSWR") == 0); // K (copy-back) only after the previous W
    CHECK(fk.overlaps == 0);
    CHECK(e.stats.strips == 6);
}

// LVGL may skip flush_wait_cb (the interrupt readied it first): the next flush takes the done itself
static void test_ppa_async_isr_first(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    CHECK(!display_engine_flush(&e, &f, &S0, false));
    CHECK(fk.ready_in_isr && e.in_flight);
    CHECK(display_engine_flush(&e, &f, &S1, true));
    CHECK(strcmp(fk.log, "SWSWR") == 0);
    CHECK(!fk.ready_in_isr && fk.overlaps == 0);
}

static void test_refused_start(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.refuse_start = true;
    CHECK(refresh(&e, &f, 2) == 2);
    CHECK(strcmp(fk.log, "CCR") == 0); // the refused strip is turned on the CPU, the next one too
    CHECK(fk.fell_back[DISPLAY_FALLBACK_START] == 1 && fk.fell_back[DISPLAY_FALLBACK_TIMEOUT] == 0);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
    CHECK(e.stats.fallbacks == 1);
    fk.refuse_start = false;
    refresh(&e, &f, 1);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU); // stays down until selected again
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC);
}

// A transfer that never completes: bounded, the strip is redone on the CPU into the same buffer, one fallback
static void test_hung_transfer(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.never_done = true;
    CHECK(refresh(&e, &f, 3) == 2); // strip 0 in the background, its wait times out, 1 and 2 on the CPU
    CHECK(strcmp(fk.log, "STTCCCR") == 0);
    CHECK(fk.cpu_last.blk.x1 == 160);
    CHECK(fk.fell_back[DISPLAY_FALLBACK_TIMEOUT] == 1);
    CHECK(fk.overlaps == 0);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
}

// The redo goes into the buffer the transfer was writing, with the strip it was turning
static void test_hung_redo_target(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.never_done = true;
    CHECK(display_engine_flush(&e, &f, &S4, true));
    CHECK(strcmp(fk.log, "STTCR") == 0); // the swap request only after the redo
    CHECK(fk.cpu_fb_last == 1 && fk.cpu_last.blk.x1 == 320);
}

// Tags: never 0, wrap past 0, and only the current one counts
static void test_gen(void) {
    CHECK(display_engine_gen_next(0) == 1);
    CHECK(display_engine_gen_next(1) == 2);
    CHECK(display_engine_gen_next(UINT32_MAX) == 1); // 0 means "none current"
    CHECK(display_engine_done_is_current(5, 5));
    CHECK(!display_engine_done_is_current(5, 4));          // a late done of an older transfer
    CHECK(!display_engine_done_is_current(0, 0));          // nothing current, a done without a tag
    CHECK(!display_engine_done_is_current(0, 5));          // after a timeout: ignored
    CHECK(!display_engine_done_is_current(1, UINT32_MAX)); // the one before a wrap
}

// Every transfer gets its own tag
static void test_gen_per_transfer(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    display_engine_flush(&e, &f, &S0, false);
    uint32_t const g0 = fk.gen_last;
    display_engine_flush(&e, &f, &S1, true);
    CHECK(g0 != 0 && fk.gen_last != 0 && fk.gen_last != g0);
}

// After a timeout no PPA engine is re-armed until the timed-out transfer reports its own done
static void test_hung_blocks_rearm(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.never_done = true;
    display_engine_flush(&e, &f, &S0, true);
    uint32_t const hung = fk.gen_last;
    CHECK(display_engine_ppa_hung(&e));
    fk.never_done = false;
    CHECK(!display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
    CHECK(!display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU); // the read-back stays cpu
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_CPU));
    display_engine_ppa_done_seen(&e, hung + 1); // some other tag: still hung
    CHECK(display_engine_ppa_hung(&e) && !display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
    display_engine_ppa_done_seen(&e, hung); // its own done: the PPA is idle
    CHECK(!display_engine_ppa_hung(&e));
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
    fk.n = 0;
    refresh(&e, &f, 1);
    CHECK(strcmp(fk.log, "KSWR") == 0 && fk.gen_last != hung); // K: the copy-back of the frame before
}

// A refused start leaves nothing running: re-arming is allowed at once
static void test_refused_not_hung(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.refuse_start = true;
    refresh(&e, &f, 1);
    CHECK(!display_engine_ppa_hung(&e));
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC));
}

// #475: a transfer later than the first bound but inside the late one: the CPU redo waits for its done, so the late
// transfer never writes over newer pixels. Not hung, one fallback.
static void test_late_done_waits(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.late_done = true;
    fk.strict    = true;
    CHECK(
        refresh(&e, &f, 2) == 1
    ); // strip 0 in the background (readied by the redo path, not counted), strip 1 on the CPU
    CHECK(strcmp(fk.log, "STLCCR") == 0); // the redo (C) only after the late done (L)
    CHECK(fk.overlaps == 0);
    CHECK(!display_engine_ppa_hung(&e));
    CHECK(fk.fell_back[DISPLAY_FALLBACK_LATE] == 1 && display_engine_active(&e) == DISPLAY_ENGINE_CPU);
}

// The same on the last (foreground) strip: the redo waits for the late done, and only then the swap is requested
static void test_late_done_last_strip(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.late_done = true;
    fk.strict    = true;
    CHECK(refresh(&e, &f, 1) == 1);
    CHECK(strcmp(fk.log, "STLCR") == 0);
    CHECK(fk.overlaps == 0 && !display_engine_ppa_hung(&e));
}

// One late fallback: on the CPU for DISPLAY_ENGINE_REARM_US, then ppa-async again at the first strip of a refresh
static void late_fallback(fake_t *fk, display_engine_t *e, display_flip_t *f) {
    fk->late_done = true;
    refresh(e, f, 2);
    fk->late_done = false;
}

static void test_late_rearm(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    late_fallback(&fk, &e, &f);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU && e.rearm);
    fk.now += DISPLAY_ENGINE_REARM_US - 100000;
    refresh(&e, &f, 1);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU && e.rearms == 0); // not yet
    fk.now += 200000;
    fk.n    = 0;
    refresh(&e, &f, 2);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC && e.rearms == 1);
    CHECK(fk.fell_back[DISPLAY_EVENT_REARM] == 1);
    CHECK(strstr(fk.log, "SW") != NULL && !e.rearm);
}

// Never done: hung, the CPU for the boot
static void test_hung_no_rearm(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    fk.never_done = true;
    refresh(&e, &f, 2);
    CHECK(display_engine_ppa_hung(&e) && !e.rearm);
    fk.never_done  = false;
    fk.now        += 60u * 1000000u;
    refresh(&e, &f, 2);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU && e.rearms == 0);
}

// The third late fallback of a boot is final
static void test_late_cap(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    for (int i = 0; i < DISPLAY_ENGINE_LATE_MAX - 1; i++) {
        late_fallback(&fk, &e, &f);
        fk.now += DISPLAY_ENGINE_REARM_US + 1000;
        refresh(&e, &f, 1);
        CHECK(display_engine_active(&e) == DISPLAY_ENGINE_PPA_ASYNC);
    }
    late_fallback(&fk, &e, &f);
    CHECK(fk.fell_back[DISPLAY_FALLBACK_LATE_FINAL] == 1 && !e.rearm);
    fk.now += 600u * 1000000u;
    refresh(&e, &f, 2);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU);
    CHECK(e.rearms == DISPLAY_ENGINE_LATE_MAX - 1 && e.late_fallbacks == DISPLAY_ENGINE_LATE_MAX);
}

// A disp.engine cpu set by hand (developer mode) is not overridden by the re-arm
static void test_manual_cpu_stands(void) {
    fake_t           fk;
    display_engine_t e;
    display_flip_t   f;
    setup(&fk, &e, &f);
    display_engine_select(&e, DISPLAY_ENGINE_PPA_ASYNC);
    late_fallback(&fk, &e, &f);
    CHECK(display_engine_select(&e, DISPLAY_ENGINE_CPU));
    fk.now += 60u * 1000000u;
    refresh(&e, &f, 2);
    CHECK(display_engine_active(&e) == DISPLAY_ENGINE_CPU && e.rearms == 0);
}

// The video present (#560): bands + a scaled strip in one frame, the video rectangle never carried
static display_strip_t const BAND_L = {{0, 0, 719, 79}, {0, 0, 79, 719}, NULL, 1440, 0, 0, 0, {0, 0, 0, 0}};
static display_strip_t const BAND_R = {{0, 640, 719, 719}, {640, 0, 719, 719}, NULL, 1440, 0, 0, 0, {0, 0, 0, 0}};
static display_strip_t const VIDEO  = {{0, 91, 719, 627}, {91, 0, 627, 719}, NULL, 640, 36, 43, 200, {0, 0, 319, 199}};

static void video_setup(fake_t *fk, display_engine_t *e, display_flip_t *f, display_video_owner_t *o) {
    setup(fk, e, f);
    display_engine_select(e, DISPLAY_ENGINE_PPA_ASYNC);
    display_video_owner_init(o);
    display_video_acquire(o);
}

static void test_strip_params(void) {
    display_ppa_params_t p;
    display_strip_ppa_params(&VIDEO, &p);
    CHECK(display_strip_scaled(&VIDEO));
    CHECK(p.pic_w == 320 && p.pic_h == 200 && p.blk_w == 320 && p.blk_h == 200 && p.off_x == 0 && p.off_y == 0);
    CHECK(p.scale_x == 2.25f && p.scale_y == 2.6875f);
    display_strip_t crop = VIDEO;
    crop.crop            = (display_rect_t){10, 20, 309, 179};
    display_strip_ppa_params(&crop, &p);
    CHECK(p.off_x == 10 && p.off_y == 20 && p.blk_w == 300 && p.blk_h == 160 && p.pic_h == 200);
    display_strip_ppa_params(&S1, &p); // the ordinary strip: what the PPA op always got
    CHECK(!display_strip_scaled(&S1));
    CHECK(p.pic_w == 720 && p.pic_h == 80 && p.blk_w == 720 && p.blk_h == 80 && p.off_x == 0 && p.off_y == 0);
    CHECK(p.scale_x == 1.0f && p.scale_y == 1.0f);
}

static void test_present_order_and_carry(void) {
    fake_t               fk;
    display_engine_t     e;
    display_flip_t       f;
    display_video_owner_t o;
    video_setup(&fk, &e, &f, &o);
    display_strip_t const bands[2] = {BAND_L, BAND_R};
    CHECK(display_engine_present(&e, &f, &o, bands, 2, &VIDEO) == DISPLAY_PRESENT_OK);
    CHECK(strcmp(fk.log, "SWSWSWR") == 0 && fk.overlaps == 0);
    CHECK(display_engine_present(&e, &f, &o, bands, 2, &VIDEO) == DISPLAY_PRESENT_OK);
    CHECK(strcmp(fk.log, "SWSWSWRKKSWSWSWR") == 0); // the bands of the first present carried, band-only
    CHECK(fk.copied_px == 2 * 80 * 720);            // never the 537 x 720 video rectangle
    CHECK(f.stats.uncarried == 2);
}

static void test_present_held_and_refused(void) {
    fake_t               fk;
    display_engine_t     e;
    display_flip_t       f;
    display_video_owner_t o;
    video_setup(&fk, &e, &f, &o);
    CHECK(display_engine_present(&e, &f, &o, &BAND_L, 1, NULL) == DISPLAY_PRESENT_HELD);
    CHECK(fk.log[0] == '\0'); // nothing written, nothing requested
    display_video_release(&o);
    CHECK(display_engine_present(&e, &f, &o, &BAND_L, 1, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(fk.log[0] == '\0');
}

// The CPU has no scaler: refused before anything is written, even at the bands
static void test_present_cpu_refuses(void) {
    fake_t               fk;
    display_engine_t     e;
    display_flip_t       f;
    display_video_owner_t o;
    setup(&fk, &e, &f); // the CPU engine
    display_video_owner_init(&o);
    display_video_acquire(&o);
    CHECK(display_engine_present(&e, &f, &o, &BAND_L, 1, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(fk.log[0] == '\0' && fk.cpu_fb_last == -1);
    // an ordinary strip still goes through the CPU
    display_engine_t e2;
    display_engine_init(&e2, &(display_engine_ops_t){fake_cpu, fake_start, fake_wait, fake_fell_back, fake_now, &fk});
    CHECK(display_engine_flush(&e2, &f, &S0, true));
    CHECK(fk.log[0] == 'C');
}

// The PPA refuses the video strip: the fallback must not draw it on the CPU, and no swap is requested
static void test_present_ppa_failure(void) {
    fake_t               fk;
    display_engine_t     e;
    display_flip_t       f;
    display_video_owner_t o;
    video_setup(&fk, &e, &f, &o);
    fk.refuse_start = true;
    CHECK(display_engine_present(&e, &f, &o, NULL, 0, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(strchr(fk.log, 'C') == NULL && strchr(fk.log, 'R') == NULL);
    CHECK(fk.fell_back[DISPLAY_FALLBACK_START] == 1);
    // a hung transfer: the redo on the CPU is skipped for the scaled strip as well
    video_setup(&fk, &e, &f, &o);
    fk.never_done = true;
    CHECK(display_engine_present(&e, &f, &o, NULL, 0, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(strchr(fk.log, 'C') == NULL && strchr(fk.log, 'R') == NULL);
    // the failure is in a band strip (#561 review): the fall-back turns that 1:1 band on the CPU, the scaled video then
    // finds the CPU engine and is refused; no frame end either way, so no half frame is shown
    video_setup(&fk, &e, &f, &o);
    fk.refuse_start = true;
    CHECK(display_engine_present(&e, &f, &o, &BAND_L, 1, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(strchr(fk.log, 'R') == NULL && fk.fell_back[DISPLAY_FALLBACK_START] == 1);
    video_setup(&fk, &e, &f, &o);
    fk.never_done = true;
    CHECK(display_engine_present(&e, &f, &o, &BAND_L, 1, &VIDEO) == DISPLAY_PRESENT_REFUSED);
    CHECK(strchr(fk.log, 'R') == NULL);
}

int main(void) {
    test_strip_params();
    test_present_order_and_carry();
    test_present_held_and_refused();
    test_present_cpu_refuses();
    test_present_ppa_failure();
    test_gen();
    test_gen_per_transfer();
    test_hung_blocks_rearm();
    test_refused_not_hung();
    test_names();
    test_cpu_default();
    test_select();
    test_ppa_async_order();
    test_ppa_async_isr_first();
    test_refused_start();
    test_hung_transfer();
    test_hung_redo_target();
    test_late_done_waits();
    test_late_done_last_strip();
    test_late_rearm();
    test_hung_no_rearm();
    test_late_cap();
    test_manual_cpu_stands();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("display_engine_test: all passed\n");
    return 0;
}
