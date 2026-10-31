// Host test: hold-Esc-to-leave timing for the keyboard test (#81)
#include "hold_exit.h"

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
    hold_exit_t h = {0};

    // A short press leaves nothing behind
    hold_exit_event(&h, true, true, 1000);
    CHECK(!hold_exit_tick(&h, 1500));
    CHECK(hold_exit_percent(&h, 1500) == 25);
    hold_exit_event(&h, true, false, 1600);
    CHECK(!hold_exit_tick(&h, 4000));
    CHECK(hold_exit_percent(&h, 4000) == 0);

    // Held: fires once, at 2 s
    hold_exit_event(&h, true, true, 5000);
    CHECK(!hold_exit_tick(&h, 6999));
    CHECK(hold_exit_tick(&h, 7000));
    CHECK(!hold_exit_tick(&h, 7100));
    CHECK(hold_exit_percent(&h, 9000) == 100);

    // A second press event while down does not restart the hold; other keys do not count
    hold_exit_event(&h, true, false, 7200);
    hold_exit_event(&h, true, true, 8000);
    hold_exit_event(&h, true, true, 9000);
    hold_exit_event(&h, false, true, 9500);
    hold_exit_event(&h, false, false, 9600);
    CHECK(hold_exit_tick(&h, 10000));

    // Another key's release does not cancel the hold
    hold_exit_event(&h, true, false, 10100);
    hold_exit_event(&h, true, true, 11000);
    hold_exit_event(&h, false, false, 11500);
    CHECK(hold_exit_tick(&h, 13000));

    // The tick counter wrapping while Esc is down
    hold_exit_event(&h, true, false, 13100);
    hold_exit_event(&h, true, true, UINT32_MAX - 500);
    CHECK(!hold_exit_tick(&h, 1000));
    CHECK(hold_exit_tick(&h, 1600));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("hold_exit_test: ok\n");
    return 0;
}
