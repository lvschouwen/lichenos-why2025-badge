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

#define CLOCK_RECOVER_ANSWERS 3  // this many big steps in a row that agree...
#define CLOCK_RECOVER_AGREE_S 60 // ...within this many seconds of each other are believed after all

// CLOCK_RECOVERED: accepted too, a big step that several answers in a row agreed on
typedef enum { CLOCK_OK, CLOCK_TOO_OLD, CLOCK_TOO_NEW, CLOCK_BIG_STEP, CLOCK_RECOVERED } clock_verdict_t;

typedef struct {
    int     count;  // consecutive big-step answers that agreed so far
    int64_t offset; // the offset (answer minus clock) of the latest of them
} clock_guard_t;

// true when t lies in build_epoch - 1 day .. build_epoch + 2 years
bool clock_in_window(int64_t t, int64_t build_epoch);

// An SNTP answer cand against the current time now. synced: SNTP has already set the time since this boot, so a jump
// of more than 10 minutes is refused (an on-path attacker could otherwise move the clock later; real drift is ms).
clock_verdict_t clock_check(int64_t cand, int64_t now, int64_t build_epoch, bool synced);

// clock_check() plus the way out of a lock-in: the first answer after a sync may have been wrong (inside the window),
// and every right answer after it is a "big step". CLOCK_RECOVERED after CLOCK_RECOVER_ANSWERS agreeing big steps in a
// row; a small step, an implausible answer or an answer that disagrees restarts the count, so one rogue answer never
// moves the clock.
clock_verdict_t clock_check_guarded(clock_guard_t *g, int64_t cand, int64_t now, int64_t build_epoch, bool synced);

static inline bool clock_accepted(clock_verdict_t v) { return v == CLOCK_OK || v == CLOCK_RECOVERED; }

char const *clock_verdict_text(clock_verdict_t v);

#ifdef __cplusplus
}
#endif
