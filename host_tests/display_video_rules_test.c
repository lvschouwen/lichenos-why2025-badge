// Host tests for the OS video surface's refusal rules and the pump's choice (#561).
#include "display_video_rules.h"

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

static display_video_acquire_in_t good(void) {
    return (display_video_acquire_in_t){
        .owned = false, .engine_ppa = true, .engine_hung = false, .swap_on = true, .psram_mhz = 120, .geom_ok = true, .src_w = 320, .src_h = 200, .mem_ok = true};
}

static void test_verdict(void) {
    display_video_acquire_in_t in = good();
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_NONE);
    in = good(); in.owned = true;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_OWNED);
    in = good(); in.engine_ppa = false; // on the CPU fallback
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_ENGINE);
    in = good(); in.engine_hung = true;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_ENGINE);
    in = good(); in.swap_on = false;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_ENGINE);
    in = good(); in.psram_mhz = 80;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_PSRAM);
    in = good(); in.psram_mhz = 200; // faster is fine
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_NONE);
    in = good(); in.geom_ok = false;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_GEOM);
    in = good(); in.mem_ok = false;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_MEM);
}

static void test_source_cap(void) {
    display_video_acquire_in_t in = good();
    in.src_w = 1024; in.src_h = 512; // 1 MB exactly
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_NONE);
    in.src_h = 513;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_GEOM);
    in = good(); in.src_w = 2000; in.src_h = 2000; // fits the panel at 0.36 per the geometry, 8 MB a buffer
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_GEOM);
    in = good(); in.src_w = 1025; in.src_h = 100;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_GEOM);
    in = good(); in.src_w = 100; in.src_h = 1025;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_GEOM);
}

static void test_verdict_order(void) {
    display_video_acquire_in_t in = {0}; // everything wrong: the first rule names it
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_ENGINE);
    in.owned = true;
    CHECK(display_video_acquire_verdict(&in) == DISPLAY_VIDEO_REFUSE_OWNED);
    CHECK(strcmp(display_video_refusal_name(DISPLAY_VIDEO_REFUSE_PSRAM), "psram-80mhz") == 0);
    CHECK(strcmp(display_video_refusal_name(DISPLAY_VIDEO_REFUSE_UI), "ui-busy") == 0);
    CHECK(strcmp(display_video_refusal_name((display_video_refusal_t)99), "?") == 0);
}

static void test_pick(void) {
    CHECK(display_video_pump_pick(1, 0, 0, 0) == 1);       // a new frame always
    CHECK(display_video_pump_pick(0, 1, 3, 0) == 0);       // and it wins over the keep-alive
    CHECK(display_video_pump_pick(-1, 0, 0, 999999) == -1); // nothing waits: idle, however long ago
    CHECK(display_video_pump_pick(-1, 1, 2, DISPLAY_VIDEO_KEEPALIVE_US - 1) == -1); // bands wait, not yet
    CHECK(display_video_pump_pick(-1, 1, 2, DISPLAY_VIDEO_KEEPALIVE_US) == 1);      // the paused app: its last frame again
    CHECK(display_video_pump_pick(-1, -1, 1, 0) == 0);     // nothing shown yet: the black source at once
    CHECK(display_video_pump_pick(-1, -1, 0, 0) == -1);
}

int main(void) {
    test_verdict();
    test_source_cap();
    test_verdict_order();
    test_pick();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("display_video_rules_test: ok\n");
    return 0;
}
