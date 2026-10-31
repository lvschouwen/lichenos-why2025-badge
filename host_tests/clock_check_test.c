// Host test: which SNTP times the badge believes (#8)
#include "clock_check.h"

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

int main(void) {
    // window edges, +-1 s
    CHECK(clock_check(B - 86400, 0, B, false) == CLOCK_OK);
    CHECK(clock_check(B - 86400 - 1, 0, B, false) == CLOCK_TOO_OLD);
    CHECK(clock_check(B + 2 * 365 * 86400LL, 0, B, false) == CLOCK_OK);
    CHECK(clock_check(B + 2 * 365 * 86400LL + 1, 0, B, false) == CLOCK_TOO_NEW);
    CHECK(clock_check(0, 0, B, false) == CLOCK_TOO_OLD);
    CHECK(clock_check(4102444800LL, 0, B, false) == CLOCK_TOO_NEW); // year 2100
    CHECK(clock_check(2147483648LL, 0, B, false) == CLOCK_TOO_NEW); // past 2038 is outside the window, not wrapped
    CHECK(clock_check(-1, 0, B, false) == CLOCK_TOO_OLD);

    // before the first sync any jump inside the window is fine (cold boot at 1970, warm boot with drifted RTC)
    CHECK(clock_check(B + 1000, 0, B, false) == CLOCK_OK);
    CHECK(clock_check(B + 1000, B + 1000 + 86400 * 30, B, false) == CLOCK_OK);

    // after a sync only small steps
    CHECK(clock_check(B + 1000 + 600, B + 1000, B, true) == CLOCK_OK);
    CHECK(clock_check(B + 1000 + 601, B + 1000, B, true) == CLOCK_BIG_STEP);
    CHECK(clock_check(B + 1000 - 600, B + 1000, B, true) == CLOCK_OK);
    CHECK(clock_check(B + 1000 - 601, B + 1000, B, true) == CLOCK_BIG_STEP);
    CHECK(clock_check(B + 1, B + 1, B, true) == CLOCK_OK);
    // the window still applies after a sync
    CHECK(clock_check(B - 86400 - 1, B - 86400, B, true) == CLOCK_TOO_OLD);

    // window helper
    CHECK(clock_in_window(B, B));
    CHECK(!clock_in_window(B - 86401, B));
    CHECK(!clock_in_window(B + 2 * 365 * 86400LL + 1, B));

    // 2038: 64-bit arithmetic does not wrap
    CHECK(clock_check(2147483647LL + 5, 2147483647LL, 2147483000LL, true) == CLOCK_OK);

    // lock-in recovery: 3 agreeing big steps, each 30 min apart, are believed; a lone rogue answer is not
    {
        clock_guard_t g = {0, 0, 0};
        int64_t       now = B + 1000, bad = 3600, h = 3600; // the clock holds a wrong first answer, the truth is an hour on
        CHECK(clock_check_guarded(&g, now + bad, now, B, true, 100) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&g, now + bad + 5, now, B, true, 100 + h) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&g, now + bad - 20, now, B, true, 100 + 2 * h) == CLOCK_RECOVERED);
        CHECK(g.count == 0);
        // a burst: three agreeing answers within 30 minutes stay ignored, however many
        clock_guard_t k = {0, 0, 0};
        for (int i = 0; i < 20; i++) {
            CHECK(clock_check_guarded(&k, now + bad, now, B, true, 100 + i * 60) == CLOCK_BIG_STEP);
        }
        // ...but the same answers 30 min apart each are accepted on the third
        clock_guard_t m = {0, 0, 0};
        CHECK(clock_check_guarded(&m, now + bad, now, B, true, 0) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&m, now + bad, now, B, true, 1799) == CLOCK_BIG_STEP); // too soon: not counted
        CHECK(clock_check_guarded(&m, now + bad, now, B, true, 1800) == CLOCK_BIG_STEP); // 2nd
        CHECK(clock_check_guarded(&m, now + bad, now, B, true, 3599) == CLOCK_BIG_STEP); // too soon again
        CHECK(clock_check_guarded(&m, now + bad, now, B, true, 3600) == CLOCK_RECOVERED);
        // a small step resets the count
        CHECK(clock_check_guarded(&g, now + bad, now, B, true, 10 * h) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&g, now + 1, now, B, true, 11 * h) == CLOCK_OK);
        CHECK(clock_check_guarded(&g, now + bad, now, B, true, 12 * h) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&g, now + bad, now, B, true, 13 * h) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&g, now + 1, now, B, true, 14 * h) == CLOCK_OK);
        // disagreeing answers never add up
        clock_guard_t d = {0, 0, 0};
        for (int i = 0; i < 10; i++) {
            CHECK(clock_check_guarded(&d, now + 3600 + i * 200, now, B, true, i * h) == CLOCK_BIG_STEP);
        }
        // an implausible answer (outside the window) resets and is never recovered
        clock_guard_t w = {0, 0, 0};
        CHECK(clock_check_guarded(&w, now + bad, now, B, true, 0) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&w, 5, now, B, true, h) == CLOCK_TOO_OLD);
        CHECK(clock_check_guarded(&w, now + bad, now, B, true, 2 * h) == CLOCK_BIG_STEP);
        CHECK(clock_check_guarded(&w, now + bad, now, B, true, 3 * h) == CLOCK_BIG_STEP);
        // before the first sync nothing is a big step
        clock_guard_t u = {0, 0, 0};
        CHECK(clock_check_guarded(&u, now + bad, 0, B, false, 0) == CLOCK_OK);
        CHECK(clock_accepted(CLOCK_OK) && clock_accepted(CLOCK_RECOVERED) && !clock_accepted(CLOCK_BIG_STEP));
    }

    // #82: the bootstrap from contacts must not rewind a clock the reset carried over
    CHECK(!clock_accept_app_set(B + 1000, B + 4600, B));         // an hour behind a good clock: refused
    CHECK(clock_accept_app_set(B + 5000, B + 4600, B));          // forward: fine
    CHECK(!clock_accept_app_set(B + 4600, B + 4600, B));         // not forward
    CHECK(clock_accept_app_set(B + 1000, 1715770351 + 99, B));   // clock not plausible (cold boot): take it
    CHECK(!clock_accept_app_set(1715770351, B + 10, B));         // implausible candidate

    for (int v = CLOCK_OK; v <= CLOCK_RECOVERED; v++) {
        CHECK(clock_verdict_text((clock_verdict_t)v)[0] != '?');
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("clock_check_test: all passed\n");
    return 0;
}
