// Host test: slew vs step for SNTP corrections, and the boot's choice between the carried and the saved clock (#326)
#include "clock_keep.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define B 1793000000LL // some build epoch (Oct 2026)

static void test_fix(void) {
    CHECK(clock_fix_for(0) == CLOCK_FIX_SLEW);
    CHECK(clock_fix_for(48000) == CLOCK_FIX_SLEW); // the hourly +48 ms
    CHECK(clock_fix_for(-48000) == CLOCK_FIX_SLEW);
    CHECK(clock_fix_for(999999) == CLOCK_FIX_SLEW);
    CHECK(clock_fix_for(-999999) == CLOCK_FIX_SLEW);
    CHECK(clock_fix_for(1000000) == CLOCK_FIX_STEP); // 1 s and up: stepped
    CHECK(clock_fix_for(-1000000) == CLOCK_FIX_STEP);
    CHECK(clock_fix_for(62LL * 60 * 1000000) == CLOCK_FIX_STEP);
    CHECK(clock_fix_for(INT64_MIN) == CLOCK_FIX_STEP);
    CHECK(clock_fix_for(INT64_MAX) == CLOCK_FIX_STEP);
    for (int f = CLOCK_FIX_NONE; f <= CLOCK_FIX_STEP; f++) {
        CHECK(clock_fix_text((clock_fix_t)f)[0] != '?');
    }
}

// An answer during a slew: the part still pending was counted by the previous step, so it is not drift
static void test_drift(void) {
    CHECK(clock_drift_ms(48, 0) == 48);
    CHECK(clock_drift_ms(60, 12) == 48);
    CHECK(clock_drift_ms(-30, -40) == 10);
}

static void test_record(void) {
    clock_rec_t r = clock_rec_make((uint32_t)B, 5000);
    CHECK(clock_rec_valid(&r));
    clock_rec_t zero = {0};
    CHECK(!clock_rec_valid(&zero)); // registers or memory after a power-on
    clock_rec_t torn = r;
    torn.wall_s += 60; // a save the reset cut after the wall clock
    CHECK(!clock_rec_valid(&torn));
    torn = r;
    torn.rtc_s += 1;
    CHECK(!clock_rec_valid(&torn));
    clock_rec_t other = clock_rec_make((uint32_t)B + 1, 5000);
    CHECK(other.check != r.check);

    CHECK(clock_rec_estimate(&r, 5000) == B);
    CHECK(clock_rec_estimate(&r, 5045) == B + 45);         // the RTC counter ran on over the reset
    CHECK(clock_rec_estimate(&r, 5000 + 3600) == B + 3600);
    CHECK(clock_rec_estimate(&r, 5000 + 3601) == B);       // a jump that big is not trusted
    CHECK(clock_rec_estimate(&r, 3) == B);                 // the counter restarted: the save alone
}

static void test_boot_pick(void) {
    clock_rec_t r   = clock_rec_make((uint32_t)B, 5000);
    int64_t     out = -1;

    // A hard reset brought back a clock stale by the run (+13..62 min seen): the save wins
    CHECK(clock_boot_pick(B - 40 * 60, &r, 5010, B, &out) == CLOCK_BOOT_SAVED && out == B + 10);
    // A clock the reset lost (1970): the save wins
    out = -1;
    CHECK(clock_boot_pick(10, &r, 5010, B, &out) == CLOCK_BOOT_SAVED && out == B + 10);
    // A soft restart carried the clock fine: the save (up to a minute old) never moves it backwards
    out = -1;
    CHECK(clock_boot_pick(B + 50, &r, 5000, B, &out) == CLOCK_BOOT_CARRIED && out == -1);
    // Equal within the truncation margin: carried
    CHECK(clock_boot_pick(B + 10, &r, 5012, B, &out) == CLOCK_BOOT_CARRIED && out == -1);
    CHECK(clock_boot_pick(B + 9, &r, 5012, B, &out) == CLOCK_BOOT_SAVED && out == B + 12);
    // No record (power-on): carried if plausible, else neither
    clock_rec_t zero = {0};
    out              = -1;
    CHECK(clock_boot_pick(B - 600, &zero, 5, B, &out) == CLOCK_BOOT_CARRIED && out == -1);
    CHECK(clock_boot_pick(10, &zero, 5, B, &out) == CLOCK_BOOT_NONE && out == -1);
    // A valid record outside the build window (an older firmware's, a year-2100 clock) is never applied
    clock_rec_t old = clock_rec_make((uint32_t)(B - 2 * 86400), 5000);
    CHECK(clock_boot_pick(10, &old, 5000, B, &out) == CLOCK_BOOT_NONE && out == -1);
    clock_rec_t future = clock_rec_make(4102444800u, 5000);
    CHECK(clock_boot_pick(B, &future, 5000, B, &out) == CLOCK_BOOT_CARRIED && out == -1);
    for (int b = CLOCK_BOOT_NONE; b <= CLOCK_BOOT_SAVED; b++) {
        CHECK(clock_boot_text((clock_boot_t)b)[0] != '?');
    }
}

int main(void) {
    test_fix();
    test_drift();
    test_record();
    test_boot_pick();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("clock_keep_test: all passed\n");
    return 0;
}
