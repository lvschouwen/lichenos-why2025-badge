#include "lichen_clock.h"

#include <time.h>

static clock_sync_info_t info;
static clock_guard_t     guard;

int64_t clock_build_epoch(void) {
    return LICHEN_BUILD_EPOCH;
}

bool clock_is_set(void) {
    return clock_in_window((int64_t)time(NULL), LICHEN_BUILD_EPOCH);
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

clock_verdict_t clock_offer_sntp(struct timeval const *tv) {
    struct timeval cur;
    gettimeofday(&cur, NULL);
    clock_verdict_t v =
        clock_check_guarded(&guard, (int64_t)tv->tv_sec, (int64_t)cur.tv_sec, LICHEN_BUILD_EPOCH, info.synced);
    info.last_verdict = v;
    if (!clock_accepted(v)) {
        info.refused++;
        return v;
    }
    info.step_ms           = ((int64_t)tv->tv_sec - (int64_t)cur.tv_sec) * 1000 + ((int64_t)tv->tv_usec - cur.tv_usec) / 1000;
    info.step_mono_ms      = mono_ms();
    settimeofday(tv, NULL);
    info.synced            = true;
    info.last_good_s       = (int64_t)tv->tv_sec;
    info.last_good_mono_ms = info.step_mono_ms;
    return v;
}

clock_sync_info_t clock_sync_info(void) {
    return info;
}
