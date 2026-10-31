#pragma once
// Which wall-clock times to believe (#8). Pure, host-tested.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCK_BELOW_BUILD_S (24 * 3600)        // a clock a day behind the build is still plausible
#define CLOCK_ABOVE_BUILD_S (2 * 365 * 24 * 3600LL) // the firmware is not trusted to run longer than this unsynced
#define CLOCK_MAX_STEP_S    (10 * 60)          // after a good sync, later SNTP answers may only nudge the clock

typedef enum { CLOCK_OK, CLOCK_TOO_OLD, CLOCK_TOO_NEW, CLOCK_BIG_STEP } clock_verdict_t;

// true when t lies in build_epoch - 1 day .. build_epoch + 2 years
bool clock_in_window(int64_t t, int64_t build_epoch);

// An SNTP answer cand against the current time now. synced: SNTP has already set the time since this boot, so a jump
// of more than 10 minutes is refused (an on-path attacker could otherwise move the clock later; real drift is ms).
clock_verdict_t clock_check(int64_t cand, int64_t now, int64_t build_epoch, bool synced);

char const *clock_verdict_text(clock_verdict_t v);

#ifdef __cplusplus
}
#endif
