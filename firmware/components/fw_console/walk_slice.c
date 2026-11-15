// The slicing rule of walk_pace.h (#471): pure, host-tested in host_tests/walk_pace_test.c
#include "walk_pace.h"

void walk_slice_begin(walk_slice_t *s, int64_t now_us) {
    s->start_us = now_us;
    s->worst_us = 0;
    s->yields   = 0;
}

bool walk_slice_step(walk_slice_t *s, int64_t now_us) {
    int64_t ran = now_us - s->start_us;
    if (ran < 0) { // a clock that went back: count nothing, start over
        ran         = 0;
        s->start_us = now_us;
    }
    uint32_t const us = ran > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ran;
    if (us > s->worst_us) {
        s->worst_us = us;
    }
    return us >= WALK_SLICE_US;
}

void walk_slice_resume(walk_slice_t *s, int64_t now_us) {
    s->start_us = now_us;
    s->yields++;
}
