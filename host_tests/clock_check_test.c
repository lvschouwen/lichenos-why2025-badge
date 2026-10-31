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

    for (int v = CLOCK_OK; v <= CLOCK_BIG_STEP; v++) {
        CHECK(clock_verdict_text((clock_verdict_t)v)[0] != '?');
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("clock_check_test: all passed\n");
    return 0;
}
