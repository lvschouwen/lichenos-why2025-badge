#include "r16_hold.h"

uint8_t const r16_detect_level[R16_LEVELS] = {10, 40, 80};

#define TOLERANCE_PCT 15

r16_fit_t r16_classify(r16_samples_t const *s) {
    for (int i = 0; i < R16_LEVELS; i++) {
        if (s->total[i] == 0) {
            return R16_UNKNOWN;
        }
    }
    for (int i = 0; i < R16_LEVELS; i++) {
        int share = (int)((uint64_t)s->high[i] * 100 / s->total[i]);
        int diff  = share - (int)r16_detect_level[i];
        if (diff < -TOLERANCE_PCT || diff > TOLERANCE_PCT) {
            return R16_ABSENT;
        }
    }
    return R16_FITTED;
}

bool r16_may_drive(r16_fit_t stored, r16_c6_t c6) {
    return stored == R16_FITTED && (c6 == R16_C6_RESET || c6 == R16_C6_ROM);
}

const char *r16_fit_name(r16_fit_t f) {
    switch (f) {
        case R16_FITTED: return "fitted";
        case R16_ABSENT: return "not fitted";
        default: return "unknown";
    }
}
