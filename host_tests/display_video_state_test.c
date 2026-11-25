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
    display_video_owner_t const idle = in_state(DISPLAY_VIDEO_IDLE), act = in_state(DISPLAY_VIDEO_ACTIVE),
                                rec = in_state(DISPLAY_VIDEO_RECLAIMING);
    // ACTIVE, no present: band strips are held, no swap of their own
    display_video_plan_t p = display_video_plan(&act, 3, false);
    CHECK(p.take == 0 && !p.swap && !p.video);
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
    test_names();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("display_video_state_test: ok\n");
    return 0;
}
