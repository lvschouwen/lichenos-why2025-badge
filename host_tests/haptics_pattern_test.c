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
    hp_strength_t const M = HP_MEDIUM, L = HP_LOW, H = HP_HIGH;
    // Notify at medium: two 80 ms pulses with an 80 ms pause, 240 ms in all; each pulse opens with a 30 ms kick
    CHECK(hp_total_ms(HP_NOTIFY, M) == 240);
    CHECK(hp_duty_at(HP_NOTIFY, M, 0) == 255 && hp_duty_at(HP_NOTIFY, M, HP_KICK_MS - 1) == 255);
    CHECK(hp_duty_at(HP_NOTIFY, M, HP_KICK_MS) == hp_hold_duty(M) && hp_duty_at(HP_NOTIFY, M, 79) == hp_hold_duty(M));
    CHECK(hp_duty_at(HP_NOTIFY, M, 80) == 0 && hp_duty_at(HP_NOTIFY, M, 159) == 0);
    CHECK(hp_duty_at(HP_NOTIFY, M, 160) == 255 && hp_duty_at(HP_NOTIFY, M, 239) == hp_hold_duty(M)); // second kick
    CHECK(hp_duty_at(HP_NOTIFY, M, 240) == 0 && hp_done(HP_NOTIFY, M, 240) && !hp_done(HP_NOTIFY, M, 239));

    // Low is shorter (60 ms pulses), high longer (100 ms)
    CHECK(hp_total_ms(HP_NOTIFY, L) == 60 + 80 + 60 && hp_total_ms(HP_NOTIFY, H) == 100 + 80 + 100);
    CHECK(hp_duty_at(HP_NOTIFY, L, 59) != 0 && hp_duty_at(HP_NOTIFY, L, 60) == 0);
    CHECK(hp_duty_at(HP_NOTIFY, H, 99) != 0 && hp_duty_at(HP_NOTIFY, H, 100) == 0);

    CHECK(hp_total_ms(HP_TICK, M) == 40 && hp_duty_at(HP_TICK, M, 39) != 0 && hp_duty_at(HP_TICK, M, 40) == 0);
    CHECK(hp_total_ms(HP_ALERT, M) == 700 && hp_total_ms(HP_ALERT, M) > hp_total_ms(HP_NOTIFY, M));
    CHECK(hp_duty_at(HP_ALERT, M, 300) == 0 && hp_duty_at(HP_ALERT, M, 400) == 255 && hp_duty_at(HP_ALERT, M, 700) == 0);
    CHECK(hp_total_ms((hp_pattern_t)99, M) == hp_total_ms(HP_TICK, M)); // unknown = tick

    // Hold duties from the datasheet maths: 90 / 95 / 100 %, so the motor sees at least its 2.3 V minimum
    CHECK(hp_hold_duty(L) == 230 && hp_hold_duty(M) == 242 && hp_hold_duty(H) == 255);
    for (int s = 0; s < HP_STRENGTH_COUNT; s++) {
        CHECK(hp_hold_duty((hp_strength_t)s) >= 230);
        CHECK(hp_duty_at(HP_ALERT, (hp_strength_t)s, 0) == 255); // every strength opens with the kick
        CHECK(hp_duty_at(HP_TICK, (hp_strength_t)s, 1) == 255);
    }

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
