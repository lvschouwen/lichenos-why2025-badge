// Host stand-in for walk_pace.c (#471): the rule with a monotonic clock and no block, for the walks host tests run
#include "walk_pace.h"

#include <time.h>

static uint32_t s_worst_us;

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

void walk_pace_begin(walk_slice_t *s) {
    walk_slice_begin(s, now_us());
}

void walk_pace(walk_slice_t *s) {
    if (walk_slice_step(s, now_us())) {
        walk_slice_resume(s, now_us());
    }
}

void walk_pace_end(walk_slice_t *s) {
    walk_slice_step(s, now_us());
    if (s->worst_us > s_worst_us) {
        s_worst_us = s->worst_us;
    }
}

uint32_t walk_pace_worst_ms(void) {
    return s_worst_us / 1000;
}
