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

char const *clock_verdict_text(clock_verdict_t v) {
    switch (v) {
        case CLOCK_OK: return "ok";
        case CLOCK_TOO_OLD: return "older than this firmware";
        case CLOCK_TOO_NEW: return "too far in the future";
        case CLOCK_BIG_STEP: return "jump too big after a sync";
    }
    return "?";
}
