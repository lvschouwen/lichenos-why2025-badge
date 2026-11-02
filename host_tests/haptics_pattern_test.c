// Host test: vibration patterns, strength, gating and the notice rate limit (#68)
#include "haptics_pattern.h"

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
    // Notify: two 80 ms buzzes with an 80 ms pause, 240 ms in all
    CHECK(hp_total_ms(HP_NOTIFY) == 240);
    CHECK(hp_on(HP_NOTIFY, 0) && hp_on(HP_NOTIFY, 79));
    CHECK(!hp_on(HP_NOTIFY, 80) && !hp_on(HP_NOTIFY, 159));
    CHECK(hp_on(HP_NOTIFY, 160) && hp_on(HP_NOTIFY, 239));
    CHECK(!hp_on(HP_NOTIFY, 240) && hp_done(HP_NOTIFY, 240) && !hp_done(HP_NOTIFY, 239));

    CHECK(hp_total_ms(HP_TICK) == 40 && hp_on(HP_TICK, 39) && !hp_on(HP_TICK, 40));
    CHECK(hp_total_ms(HP_ALERT) == 700 && hp_total_ms(HP_ALERT) > hp_total_ms(HP_NOTIFY));
    CHECK(!hp_on(HP_ALERT, 300) && hp_on(HP_ALERT, 400) && !hp_on(HP_ALERT, 700));
    CHECK(hp_total_ms((hp_pattern_t)99) == hp_total_ms(HP_TICK)); // unknown = tick

    // Strength: the motor needs about half duty to start, high is full
    CHECK(hp_duty(HP_LOW) >= 128 && hp_duty(HP_LOW) < hp_duty(HP_MEDIUM) && hp_duty(HP_MEDIUM) < hp_duty(HP_HIGH));
    CHECK(hp_duty(HP_HIGH) == 255);

    // Setting off or an update running = silent
    CHECK(hp_allowed(true, false));
    CHECK(!hp_allowed(false, false));
    CHECK(!hp_allowed(true, true));

    CHECK(hp_for_notice(0) == HP_NOTIFY && hp_for_notice(1) == HP_NOTIFY && hp_for_notice(2) == HP_ALERT);

    // Rate limit: one buzz per 2 s, across the 32-bit wrap too
    hp_gate_t g = {0};
    CHECK(hp_gate_take(&g, 5000, HP_NOTIFY_GAP_MS));
    CHECK(!hp_gate_take(&g, 5001, HP_NOTIFY_GAP_MS));
    CHECK(!hp_gate_take(&g, 6999, HP_NOTIFY_GAP_MS));
    CHECK(hp_gate_take(&g, 7000, HP_NOTIFY_GAP_MS));
    CHECK(!hp_gate_take(&g, 8000, HP_NOTIFY_GAP_MS)); // a refused one does not move the window
    CHECK(hp_gate_take(&g, 9000, HP_NOTIFY_GAP_MS));
    hp_gate_t w = {0};
    CHECK(hp_gate_take(&w, 0xFFFFFF00u, HP_NOTIFY_GAP_MS));
    CHECK(!hp_gate_take(&w, 100, HP_NOTIFY_GAP_MS));
    CHECK(hp_gate_take(&w, 3000, HP_NOTIFY_GAP_MS));

    if (failures) {
        return 1;
    }
    printf("haptics_pattern_test: ok\n");
    return 0;
}
