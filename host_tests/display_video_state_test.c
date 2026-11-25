// Host tests for the video owner state machine and the flip rule (#559): every transition, the refused ones too.
#include "display_video_state.h"

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

static display_video_owner_t in_state(display_video_state_t s) {
    display_video_owner_t o;
    display_video_owner_init(&o);
    if (s != DISPLAY_VIDEO_IDLE)
        display_video_acquire(&o);
    if (s == DISPLAY_VIDEO_RECLAIMING)
        display_video_reclaim(&o, DISPLAY_VIDEO_RECLAIM_ESCAPE);
    return o;
}

static void test_acquire(void) {
    display_video_owner_t o = in_state(DISPLAY_VIDEO_IDLE);
    CHECK(display_video_acquire(&o) && o.state == DISPLAY_VIDEO_ACTIVE && o.acquires == 1);
    CHECK(!display_video_acquire(&o) && o.state == DISPLAY_VIDEO_ACTIVE && o.refused == 1);
    o = in_state(DISPLAY_VIDEO_RECLAIMING);
    CHECK(!display_video_acquire(&o) && o.state == DISPLAY_VIDEO_RECLAIMING);
}

static void test_reclaim(void) {
    for (int r = DISPLAY_VIDEO_RECLAIM_DIALOG; r < DISPLAY_VIDEO_RECLAIM_COUNT; r++) {
        display_video_owner_t o = in_state(DISPLAY_VIDEO_ACTIVE);
        CHECK(display_video_reclaim(&o, (display_video_reclaim_t)r));
        CHECK(o.state == DISPLAY_VIDEO_RECLAIMING && o.reason == (display_video_reclaim_t)r && o.reclaims == 1);
        CHECK(display_video_reclaim_done(&o) && o.state == DISPLAY_VIDEO_IDLE);
        CHECK(o.reason == (display_video_reclaim_t)r); // kept for the log line
    }
    display_video_owner_t o = in_state(DISPLAY_VIDEO_ACTIVE);
    CHECK(!display_video_reclaim(&o, DISPLAY_VIDEO_RECLAIM_NONE) && o.state == DISPLAY_VIDEO_ACTIVE);
    CHECK(!display_video_reclaim(&o, DISPLAY_VIDEO_RECLAIM_COUNT) && o.state == DISPLAY_VIDEO_ACTIVE);
    o = in_state(DISPLAY_VIDEO_IDLE);
    CHECK(!display_video_reclaim(&o, DISPLAY_VIDEO_RECLAIM_CARD) && o.state == DISPLAY_VIDEO_IDLE && o.reclaims == 0);
    o = in_state(DISPLAY_VIDEO_RECLAIMING);
    CHECK(!display_video_reclaim(&o, DISPLAY_VIDEO_RECLAIM_THERMAL) && o.reason == DISPLAY_VIDEO_RECLAIM_ESCAPE);
    CHECK(!display_video_reclaim_done(&(display_video_owner_t){0}));
    o = in_state(DISPLAY_VIDEO_ACTIVE);
    CHECK(!display_video_reclaim_done(&o) && o.state == DISPLAY_VIDEO_ACTIVE);
}

static void test_release(void) {
    display_video_owner_t o = in_state(DISPLAY_VIDEO_IDLE);
    CHECK(!display_video_release(&o) && o.state == DISPLAY_VIDEO_IDLE);
    o = in_state(DISPLAY_VIDEO_ACTIVE);
    CHECK(display_video_release(&o) && o.state == DISPLAY_VIDEO_IDLE);
    o = in_state(DISPLAY_VIDEO_RECLAIMING);
    CHECK(display_video_release(&o) && o.state == DISPLAY_VIDEO_IDLE);
    CHECK(display_video_acquire(&o)); // usable again
}

static void test_plan(void) {
    display_video_owner_t idle = in_state(DISPLAY_VIDEO_IDLE), act = in_state(DISPLAY_VIDEO_ACTIVE),
                          rec = in_state(DISPLAY_VIDEO_RECLAIMING);
    // ACTIVE, no present: band strips are held, no swap of their own
    display_video_plan_t p = display_video_plan(&act, 3, false);
    CHECK(p.take == 0 && p.carry == 0 && !p.swap && !p.video && act.band_last == 0);
    // ACTIVE, present: bands ride it, one swap
    p = display_video_plan(&act, 3, true);
    CHECK(p.take == 3 && p.swap && p.video);
    p = display_video_plan(&act, 0, true);
    CHECK(p.take == 0 && p.swap && p.video);
    p = display_video_plan(&act, 0, false);
    CHECK(p.take == 0 && !p.swap && !p.video);
    // Not active: strips go out at once and end the frame as before; a present is not honoured
    p = display_video_plan(&idle, 2, false);
    CHECK(p.take == 2 && p.swap && !p.video);
    p = display_video_plan(&idle, 0, false);
    CHECK(p.take == 0 && !p.swap && !p.video);
    p = display_video_plan(&idle, 2, true);
    CHECK(p.take == 2 && p.swap && !p.video);
    p = display_video_plan(&rec, 1, true);
    CHECK(p.take == 1 && p.swap && !p.video);
}

static void test_carry(void) {
    display_video_owner_t o = in_state(DISPLAY_VIDEO_ACTIVE);
    // A clock tick: one band strip pending, held until the present
    display_video_plan_t p = display_video_plan(&o, 1, false);
    CHECK(p.take == 0 && p.carry == 0);
    p = display_video_plan(&o, 1, true); // present 1 writes the band into buffer A
    CHECK(p.take == 1 && p.carry == 0 && p.swap);
    p = display_video_plan(&o, 0, true); // present 2: buffer B lacks it
    CHECK(p.take == 0 && p.carry == 1 && p.swap);
    p = display_video_plan(&o, 0, true); // present 3: both have it
    CHECK(p.carry == 0);
    // Two ticks in a row: each present carries the previous one's bands
    p = display_video_plan(&o, 2, true);
    CHECK(p.take == 2 && p.carry == 0);
    p = display_video_plan(&o, 1, true);
    CHECK(p.take == 1 && p.carry == 2);
    p = display_video_plan(&o, 0, false); // a held frame changes nothing
    CHECK(p.carry == 0 && o.band_last == 1);
    // A new acquire starts clean (the bars are painted into both buffers)
    display_video_release(&o);
    CHECK(display_video_acquire(&o) && o.band_last == 0);
}

static void test_release_reclaiming(void) {
    display_video_owner_t o = in_state(DISPLAY_VIDEO_RECLAIMING);
    CHECK(o.reason == DISPLAY_VIDEO_RECLAIM_ESCAPE);
    CHECK(display_video_release(&o) && o.reason == DISPLAY_VIDEO_RECLAIM_NONE);
    // The reclaim's own completion arriving after the release is a no-op, not a refusal
    CHECK(!display_video_reclaim_done(&o) && o.state == DISPLAY_VIDEO_IDLE);
    CHECK(o.refused == 0);
}

static void test_names(void) {
    CHECK(strcmp(display_video_state_name(DISPLAY_VIDEO_ACTIVE), "active") == 0);
    CHECK(strcmp(display_video_state_name(DISPLAY_VIDEO_IDLE), "idle") == 0);
    CHECK(strcmp(display_video_state_name(DISPLAY_VIDEO_RECLAIMING), "reclaiming") == 0);
    CHECK(strcmp(display_video_reclaim_name(DISPLAY_VIDEO_RECLAIM_ENGINE), "engine") == 0);
    CHECK(strcmp(display_video_reclaim_name(DISPLAY_VIDEO_RECLAIM_COUNT), "?") == 0);
}

int main(void) {
    test_acquire();
    test_reclaim();
    test_release();
    test_plan();
    test_carry();
    test_release_reclaiming();
    test_names();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("display_video_state_test: ok\n");
    return 0;
}
