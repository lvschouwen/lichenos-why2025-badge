#include "adv_sched.h"

#include <string.h>

void adv_sched_init(adv_sched_t *s) {
    memset(s, 0, sizeof(*s));
}

void adv_sched_restart(adv_sched_t *s) {
    s->asked = false;
    s->done  = false;
}

// The round's mode at elapsed ms into it
static adv_mode_t mode_at(uint32_t elapsed, bool bonded_only) {
    uint32_t fast = bonded_only ? 0 : ADV_FAST_MS;
    if (elapsed < fast) {
        return ADV_FAST;
    }
    return elapsed - fast < ADV_SLOW_MS ? ADV_SLOW : ADV_OFF;
}

adv_mode_t adv_sched_step(adv_sched_t *s, uint32_t now_ms, adv_wish_t w) {
    if (!w.want) {
        s->asked = false;
        return ADV_OFF;
    }
    if (!s->asked) {
        s->asked    = true;
        s->done     = false;
        s->start_ms = now_ms;
    }
    adv_mode_t m = s->done ? ADV_OFF : mode_at(now_ms - s->start_ms, w.bonded_only);
    s->done      = m == ADV_OFF;
    // a full badge stops advertising, but the round's clock runs on: room again mid-round is not a new round
    return w.room ? m : ADV_OFF;
}

uint32_t adv_sched_interval_ms(adv_mode_t mode) {
    switch (mode) {
        case ADV_FAST: return ADV_FAST_ITVL_MS;
        case ADV_SLOW: return ADV_SLOW_ITVL_MS;
        case ADV_OFF: break;
    }
    return 0;
}

uint32_t adv_sched_next_ms(adv_sched_t const *s, uint32_t now_ms, adv_wish_t w) {
    if (!w.want || !s->asked || s->done) {
        return 0;
    }
    uint32_t elapsed = now_ms - s->start_ms, fast = w.bonded_only ? 0 : ADV_FAST_MS;
    if (elapsed < fast) {
        return fast - elapsed;
    }
    return elapsed - fast < ADV_SLOW_MS ? fast + ADV_SLOW_MS - elapsed : 0;
}
