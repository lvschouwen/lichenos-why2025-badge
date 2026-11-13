// Host test: the developer-mode tap counter (#374)
#include "dev_tap.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    dev_tap_t t;
    dev_tap_reset(&t);

    // Seven quick taps: only the seventh fires
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(!dev_tap(&t, 1000 + (uint32_t)i * 500));
    }
    CHECK(dev_tap(&t, 1000 + (DEV_TAP_COUNT - 1) * 500));
    // Fired: the count starts over, an eighth tap is the first of the next run
    CHECK(!dev_tap(&t, 5000));
    CHECK(t.n == 1);

    // Exactly the gap keeps the run, one ms more drops it
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(!dev_tap(&t, (uint32_t)i * DEV_TAP_GAP_MS));
    }
    CHECK(dev_tap(&t, (DEV_TAP_COUNT - 1) * DEV_TAP_GAP_MS));
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(!dev_tap(&t, (uint32_t)i * 100));
    }
    CHECK(!dev_tap(&t, 500 + DEV_TAP_GAP_MS + 1)); // too late: this is tap 1 of a new run
    CHECK(t.n == 1);
    for (int i = 1; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(!dev_tap(&t, 5000 + (uint32_t)i));
    }
    CHECK(dev_tap(&t, 6000)); // 6 more after the late one: 7 in all

    // Reset (developer mode off) forgets a partial run
    dev_tap_reset(&t);
    for (int i = 0; i < 5; i++) {
        dev_tap(&t, (uint32_t)i);
    }
    dev_tap_reset(&t);
    CHECK(!dev_tap(&t, 10));
    CHECK(!dev_tap(&t, 11));
    CHECK(t.n == 2);

    // The tick wraps
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(!dev_tap(&t, 0xFFFFFF00u + (uint32_t)i * 100));
    }
    CHECK(dev_tap(&t, 0xFFFFFF00u + 6 * 100)); // 0xFFFFFF00+600 wraps past zero

    // Enter while on asks to switch off and never counts; off, only the seventh switches on
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(dev_enter_action(&t, false, false, (uint32_t)i) == DEV_ENTER_NONE);
    }
    CHECK(dev_enter_action(&t, false, false, 6) == DEV_ENTER_SWITCH_ON);
    CHECK(dev_enter_action(&t, true, false, 7) == DEV_ENTER_ASK_OFF);
    CHECK(t.n == 0);

    // Injected Enters (#374 H1, fw keys send over Wi-Fi): seven of them never switch on, while on they never ask to
    // switch off, and they leave a hand's count where it was
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT * 2; i++) {
        CHECK(dev_enter_action(&t, false, true, (uint32_t)i * 100) == DEV_ENTER_REFUSED);
    }
    CHECK(t.n == 0);
    CHECK(dev_enter_action(&t, true, true, 2000) == DEV_ENTER_REFUSED);
    dev_tap_reset(&t);
    for (int i = 0; i < DEV_TAP_COUNT - 1; i++) {
        CHECK(dev_enter_action(&t, false, false, 3000 + (uint32_t)i * 100) == DEV_ENTER_NONE);
    }
    CHECK(dev_enter_action(&t, false, true, 3700) == DEV_ENTER_REFUSED);
    CHECK(t.n == DEV_TAP_COUNT - 1 && t.last == 3000 + (DEV_TAP_COUNT - 2) * 100);
    CHECK(dev_enter_action(&t, false, false, 3800) == DEV_ENTER_SWITCH_ON); // the hand's seventh still counts

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("dev_tap_test ok\n");
    return 0;
}
