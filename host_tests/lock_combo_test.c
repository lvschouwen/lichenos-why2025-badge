// Host test: the pocket lock's unlock combo (#75)
#include "lock_combo.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define KEY_Q 0x16

int main(void) {
    lock_combo_t c;
    lock_combo_reset(&c);

    // One key alone does nothing
    lock_combo_event(&c, LOCK_KEY_A, true, 0);
    CHECK(!lock_combo_tick(&c, 5000));
    CHECK(lock_combo_percent(&c, 5000) == 0);

    // Both: fires once at LOCK_HOLD_MS after the second went down
    lock_combo_event(&c, LOCK_KEY_B, true, 6000);
    CHECK(lock_combo_percent(&c, 6500) == 50);
    CHECK(!lock_combo_tick(&c, 6000 + LOCK_HOLD_MS - 1));
    CHECK(lock_combo_tick(&c, 6000 + LOCK_HOLD_MS));
    CHECK(!lock_combo_tick(&c, 9000)); // still held: no second fire
    CHECK(lock_combo_percent(&c, 9000) == 0);

    // Releasing one and pressing it again does not re-arm; releasing both does
    lock_combo_event(&c, LOCK_KEY_A, false, 9100);
    lock_combo_event(&c, LOCK_KEY_A, true, 9200);
    CHECK(!lock_combo_tick(&c, 12000));
    lock_combo_event(&c, LOCK_KEY_A, false, 12100);
    lock_combo_event(&c, LOCK_KEY_B, false, 12100);
    lock_combo_event(&c, LOCK_KEY_B, true, 13000);
    lock_combo_event(&c, LOCK_KEY_A, true, 13000);
    CHECK(lock_combo_tick(&c, 13000 + LOCK_HOLD_MS));
    lock_combo_event(&c, LOCK_KEY_A, false, 15000);
    lock_combo_event(&c, LOCK_KEY_B, false, 15000);

    // A third key down (a pocket clump) stops the count; letting it go starts it again from then
    lock_combo_event(&c, LOCK_KEY_A, true, 20000);
    lock_combo_event(&c, LOCK_KEY_B, true, 20000);
    lock_combo_event(&c, KEY_Q, true, 20500);
    CHECK(!lock_combo_tick(&c, 21500));
    lock_combo_event(&c, KEY_Q, false, 21600);
    CHECK(!lock_combo_tick(&c, 21600 + LOCK_HOLD_MS - 1));
    CHECK(lock_combo_tick(&c, 21600 + LOCK_HOLD_MS));
    lock_combo_event(&c, LOCK_KEY_A, false, 30000);
    lock_combo_event(&c, LOCK_KEY_B, false, 30000);

    // Pressed together with another key from the start: nothing
    lock_combo_event(&c, KEY_Q, true, 40000);
    lock_combo_event(&c, LOCK_KEY_A, true, 40000);
    lock_combo_event(&c, LOCK_KEY_B, true, 40000);
    CHECK(!lock_combo_tick(&c, 50000));

    // A release of a key never seen pressed (held before the lock listened) is harmless; out-of-range codes ignored
    lock_combo_reset(&c);
    lock_combo_event(&c, KEY_Q, false, 100);
    lock_combo_event(&c, 0xFF, true, 100);
    lock_combo_event(&c, LOCK_KEY_A, true, 200);
    lock_combo_event(&c, LOCK_KEY_B, true, 200);
    CHECK(lock_combo_tick(&c, 200 + LOCK_HOLD_MS));

    // Wrap-safe ticks
    lock_combo_reset(&c);
    lock_combo_event(&c, LOCK_KEY_A, true, UINT32_MAX - 100);
    lock_combo_event(&c, LOCK_KEY_B, true, UINT32_MAX - 100);
    CHECK(!lock_combo_tick(&c, 500));
    CHECK(lock_combo_tick(&c, LOCK_HOLD_MS - 100));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lock_combo_test ok\n");
    return 0;
}
