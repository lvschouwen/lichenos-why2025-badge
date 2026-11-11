#pragma once
// Keeping the clock good between and across resets (#326). Pure, host-tested (host_tests/clock_keep_test.c):
// - how an accepted SNTP answer is applied: a small offset is slewed (adjtime), a large one stepped;
// - the clock record saved every minute and what the boot does with it.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Offsets below 1 s are slewed. IDF 6.1's adjtime runs the clock 1/64 fast or slow until the offset is made up: the
// hourly +40..+55 ms takes ~3 s, 1 s about a minute, and time never jumps or runs backwards. Anything larger (the
// first answer after a cold boot, a recovered big step) is stepped as before.
#define CLOCK_SLEW_MAX_US 1000000LL

typedef enum { CLOCK_FIX_NONE, CLOCK_FIX_SLEW, CLOCK_FIX_STEP } clock_fix_t;

// SLEW when |offset_us| < CLOCK_SLEW_MAX_US, else STEP
clock_fix_t clock_fix_for(int64_t offset_us);
// "slewed", "stepped", "none"
char const *clock_fix_text(clock_fix_t f);

// The clock's own drift between two SNTP answers: the measured offset minus the correction still pending from the
// previous one when the answer came (pending_ms: adjtime's remainder; the previous step already counted it, so it is
// not drift). Both the next slew and a step drop that remainder: the new offset is measured against the clock as shown.
int64_t clock_drift_ms(int64_t step_ms, int64_t pending_ms);

// The record saved every CLOCK_SAVE_EVERY_S while the clock is set: the wall clock and the RTC counter then (both
// seconds) and a check word, so a record a reset tore in half or memory that held garbage is never believed
#define CLOCK_SAVE_EVERY_S       60
#define CLOCK_SAVE_MARGIN_S      2    // a saved estimate must beat the carried clock by this much (both are truncated)
#define CLOCK_SAVE_ELAPSED_MAX_S 3600 // RTC time since the save counted at boot only up to this (else the save alone)

typedef struct {
    uint32_t wall_s;
    uint32_t rtc_s;
    uint32_t check;
} clock_rec_t;

clock_rec_t clock_rec_make(uint32_t wall_s, uint32_t rtc_s);
bool        clock_rec_valid(clock_rec_t const *r);
// The time the record stands for now: the save plus the RTC counter since it, when the counter ran on (0..1 h);
// a counter that restarted or jumped adds nothing
int64_t clock_rec_estimate(clock_rec_t const *r, int64_t rtc_now_s);

typedef enum { CLOCK_BOOT_NONE, CLOCK_BOOT_CARRIED, CLOCK_BOOT_SAVED } clock_boot_t;

// The boot's choice between the clock the reset carried (carried_s, time(NULL) at boot) and the record r: the saved
// estimate only when the record is valid, the estimate lies in the build window and it is newer than a plausible
// carried clock by more than CLOCK_SAVE_MARGIN_S. A save never moves the clock backwards. *out_s: the time to set
// for SAVED (untouched otherwise).
clock_boot_t clock_boot_pick(int64_t carried_s, clock_rec_t const *r, int64_t rtc_now_s, int64_t build_epoch,
                             int64_t *out_s);
// "saved", "carried", "neither"
char const *clock_boot_text(clock_boot_t b);

#ifdef __cplusplus
}
#endif
