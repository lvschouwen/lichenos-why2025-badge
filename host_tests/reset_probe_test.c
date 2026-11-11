// Host test: the reset-survival probe's marker and boot line (#330)
#include "reset_probe_logic.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_marker(void) {
    reset_probe_word_t w = reset_probe_make(0);
    CHECK(reset_probe_ok(&w));
    reset_probe_word_t zero = {0};
    CHECK(!reset_probe_ok(&zero)); // a cleared store
    reset_probe_word_t ones = {0xffffffffu, 0xffffffffu, 0xffffffffu};
    CHECK(!reset_probe_ok(&ones));
    reset_probe_word_t half = w;
    half.count              = 7; // a reset between the stores of a rewrite
    CHECK(!reset_probe_ok(&half));
    reset_probe_word_t bad = w;
    bad.magic ^= 1u;
    CHECK(!reset_probe_ok(&bad));
}

static void test_next(void) {
    reset_probe_word_t w = reset_probe_make(4);
    reset_probe_word_t n = reset_probe_next(&w);
    CHECK(reset_probe_ok(&n) && n.count == 5); // survived: one more
    reset_probe_word_t lost = {0};
    n                       = reset_probe_next(&lost);
    CHECK(reset_probe_ok(&n) && n.count == 0); // lost: re-armed, so the next reset is a test again
}

static void test_line(void) {
    char           line[128];
    bool const     ok[RESET_PROBE_STORES]    = {true, false, true};
    uint32_t const count[RESET_PROBE_STORES] = {3, 0, 12};
    CHECK(strcmp(reset_probe_line(line, sizeof(line), "SW", 3, ok, count),
                 "reset SW (rom 0x3), rtc ok, lp_store lost, psram ok (survived 3/0/12)") == 0);
    CHECK(strcmp(reset_probe_line(line, sizeof(line), NULL, 0x7, ok, count),
                 "reset ? (rom 0x7), rtc ok, lp_store lost, psram ok (survived 3/0/12)") == 0);
    char tiny[6];
    CHECK(strcmp(reset_probe_line(tiny, sizeof(tiny), "SW", 3, ok, count), "reset") == 0);
    CHECK(reset_probe_line(tiny, 0, "SW", 3, ok, count) == tiny);
}

int main(void) {
    test_marker();
    test_next();
    test_line();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reset_probe_test: all passed\n");
    return 0;
}
