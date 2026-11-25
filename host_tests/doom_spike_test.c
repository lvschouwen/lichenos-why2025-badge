// Host test: the DOOM spike's engine-only timing (#558): work = frame time minus the sleeps inside it, one clock
#include "doom_spike.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint32_t clk; // units; 1000 per ms, as a cycle counter calibrated at 1 MHz would be

static uint32_t fake_now(void) {
    return clk;
}

static void ms(uint32_t n) {
    clk += n * 1000;
}

// A frame of work_ms with a sleep_ms wait inside it, every timestamp from the module's own clock
static void frame(doom_spike_t *s, uint32_t work_ms, uint32_t sleep_ms, bool level) {
    doom_spike_frame_begin(s);
    ms(work_ms);
    doom_spike_sleep_begin(s);
    ms(sleep_ms);
    doom_spike_sleep_end(s);
    doom_spike_frame_end(s, level);
}

static void work_excludes_sleep(void) {
    doom_spike_t s;
    clk = 123456789; // not near zero: a clock mixed with another unit would not cancel out
    doom_spike_init(&s, fake_now, 1000);
    frame(&s, 10, 20, true);
    CHECK(s.level_frames == 1 && s.work_units == 10000 && s.max_units == 10000);
    frame(&s, 0, 28, true); // a frame that only waited has no work, not an underflow
    CHECK(s.work_units == 10000 && s.max_units == 10000 && s.level_frames == 2);
}

static void thresholds_and_level(void) {
    doom_spike_t s;
    doom_spike_init(&s, fake_now, 1000);
    frame(&s, 34, 0, true);
    frame(&s, 51, 0, true);
    frame(&s, 33, 0, true); // exactly 33 ms is not over
    frame(&s, 99, 0, false); // a title or load frame: counted as a frame only
    CHECK(s.frames == 4 && s.level_frames == 3 && s.over33 == 2 && s.over50 == 1 && s.max_units == 51000);
}

static void format_line(void) {
    doom_spike_t s;
    char         out[160];
    doom_spike_init(&s, fake_now, 1000);
    doom_spike_format(&s, out, sizeof(out), 0, true, 1000);
    CHECK(strstr(out, "no level frame in 0 frames"));
    frame(&s, 10, 0, true);
    frame(&s, 15, 0, true);
    doom_spike_format(&s, out, sizeof(out), 1, true, 1000);
    CHECK(strstr(out, "core 1 cyc: avg 12.5 max 15.0 ms, >33 ms 0, >50 ms 0 of 2 level frames, 2 fps wall"));
    doom_spike_format(&s, out, sizeof(out), 0, false, 0);
    CHECK(strstr(out, "tick") && strstr(out, "0 fps wall"));
}

static void unit_zero_is_one(void) {
    doom_spike_t s;
    doom_spike_init(&s, fake_now, 0);
    CHECK(s.unit_per_ms == 1);
}

int main(void) {
    work_excludes_sleep();
    thresholds_and_level();
    format_line();
    unit_zero_is_one();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("doom_spike_test: ok\n");
    return 0;
}
