#include "clock_check.h"

bool clock_in_window(int64_t t, int64_t build_epoch) {
    return t >= build_epoch - CLOCK_BELOW_BUILD_S && t <= build_epoch + CLOCK_ABOVE_BUILD_S;
}

clock_verdict_t clock_check(int64_t cand, int64_t now, int64_t build_epoch, bool synced) {
    if (cand < build_epoch - CLOCK_BELOW_BUILD_S) {
        return CLOCK_TOO_OLD;
    }
    if (cand > build_epoch + CLOCK_ABOVE_BUILD_S) {
        return CLOCK_TOO_NEW;
    }
    if (synced) {
        int64_t step = cand - now;
        if (step > CLOCK_MAX_STEP_S || step < -CLOCK_MAX_STEP_S) {
            return CLOCK_BIG_STEP;
        }
    }
    return CLOCK_OK;
}

clock_verdict_t clock_check_guarded(clock_guard_t *g, int64_t cand, int64_t now, int64_t build_epoch, bool synced,
                                    int64_t mono_s) {
    clock_verdict_t v = clock_check(cand, now, build_epoch, synced);
    if (v != CLOCK_BIG_STEP) {
        g->count = 0;
        return v;
    }
    int64_t off  = cand - now;
    int64_t diff = off - g->offset;
    if (g->count > 0 && diff <= CLOCK_RECOVER_AGREE_S && diff >= -CLOCK_RECOVER_AGREE_S) {
        if (mono_s - g->at_s < CLOCK_RECOVER_SPACE_S) {
            return CLOCK_BIG_STEP; // too soon after the last counted answer: not counted
        }
        g->count++;
    } else {
        g->count = 1;
    }
    g->offset = off;
    g->at_s   = mono_s;
    if (g->count >= CLOCK_RECOVER_ANSWERS) {
        g->count = 0;
        return CLOCK_RECOVERED;
    }
    return CLOCK_BIG_STEP;
}

char const *clock_verdict_text(clock_verdict_t v) {
    switch (v) {
        case CLOCK_OK: return "ok";
        case CLOCK_TOO_OLD: return "older than this firmware";
        case CLOCK_TOO_NEW: return "too far in the future";
        case CLOCK_BIG_STEP: return "jump too big after a sync";
        case CLOCK_RECOVERED: return "big step confirmed by repeated answers";
    }
    return "?";
}
