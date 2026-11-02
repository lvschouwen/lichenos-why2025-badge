// Host test: R16 detection and the rule for driving GPIO5 (#12)
#include "r16_hold.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static r16_samples_t mk(uint32_t a, uint32_t b, uint32_t c) {
    r16_samples_t s = {.high = {a, b, c}, .total = {1000, 1000, 1000}};
    return s;
}

int main(void) {
    r16_samples_t fitted = mk(100, 400, 800);
    CHECK(r16_classify(&fitted) == R16_FITTED);
    r16_samples_t edge_in = mk(250, 400, 800), edge_out = mk(260, 400, 800); // 15 points off is still fitted
    CHECK(r16_classify(&edge_in) == R16_FITTED);
    CHECK(r16_classify(&edge_out) == R16_ABSENT);
    r16_samples_t low = mk(0, 0, 0), high = mk(1000, 1000, 1000), half = mk(500, 500, 500), noise = mk(300, 700, 200);
    CHECK(r16_classify(&low) == R16_ABSENT); // pin pulled low: stock badge
    CHECK(r16_classify(&high) == R16_ABSENT);
    CHECK(r16_classify(&half) == R16_ABSENT);
    CHECK(r16_classify(&noise) == R16_ABSENT);
    r16_samples_t inverted = mk(800, 400, 100);
    CHECK(r16_classify(&inverted) == R16_ABSENT);
    r16_samples_t none = {0};
    CHECK(r16_classify(&none) == R16_UNKNOWN);
    r16_samples_t partial = mk(100, 400, 800);
    partial.total[2] = 0;
    CHECK(r16_classify(&partial) == R16_UNKNOWN);

    // Never drive unless fitted, and never while the C6 application may run
    for (int fit = R16_UNKNOWN; fit <= R16_ABSENT; fit++) {
        for (int c6 = R16_C6_APP; c6 <= R16_C6_ROM; c6++) {
            bool expect = fit == R16_FITTED && c6 != R16_C6_APP;
            CHECK(r16_may_drive((r16_fit_t)fit, (r16_c6_t)c6) == expect);
        }
    }
    CHECK(!r16_may_drive(R16_ABSENT, R16_C6_ROM));
    CHECK(!r16_may_drive(R16_UNKNOWN, R16_C6_RESET));
    CHECK(!r16_may_drive(R16_FITTED, R16_C6_APP));

    printf(failures ? "r16_hold_test: %d failures\n" : "r16_hold_test: ok\n", failures);
    return failures != 0;
}
