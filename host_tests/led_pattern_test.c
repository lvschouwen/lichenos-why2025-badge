// Host test: the status LED's pattern choice and timing (#74)
#include "led_pattern.h"

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
    led_inputs_t in = {.enabled = true};
    CHECK(led_pick(&in) == LED_OFF);

    in.unread = true;
    CHECK(led_pick(&in) == LED_PULSE);
    in.dnd = true; // do-not-disturb: notices stay dark
    CHECK(led_pick(&in) == LED_OFF);
    in.dnd = false;
    in.enabled = false; // setting off
    CHECK(led_pick(&in) == LED_OFF);
    in.enabled = true;
    in.unread  = false; // all read clears
    CHECK(led_pick(&in) == LED_OFF);

    in.updating = true;
    CHECK(led_pick(&in) == LED_FAST);
    in.dnd = true; // an update still shows
    CHECK(led_pick(&in) == LED_FAST);
    in.enabled = false;
    CHECK(led_pick(&in) == LED_OFF);

    // The console override wins over everything, and auto gives the inputs back
    in.override = LED_FORCE_ON;
    CHECK(led_pick(&in) == LED_ON);
    in.override = LED_FORCE_BLINK;
    CHECK(led_pick(&in) == LED_SLOW);
    in.override = LED_FORCE_OFF;
    in.enabled  = true;
    CHECK(led_pick(&in) == LED_OFF);
    in.override = LED_AUTO;
    CHECK(led_pick(&in) == LED_FAST);

    // Timing
    CHECK(!led_level(LED_OFF, 0) && led_level(LED_ON, 12345));
    CHECK(led_level(LED_PULSE, 0) && led_level(LED_PULSE, LED_PULSE_ON_MS - 1));
    CHECK(!led_level(LED_PULSE, LED_PULSE_ON_MS) && !led_level(LED_PULSE, LED_PULSE_PERIOD_MS - 1));
    CHECK(led_level(LED_PULSE, LED_PULSE_PERIOD_MS * 3));
    CHECK(led_level(LED_SLOW, 499) && !led_level(LED_SLOW, 500) && led_level(LED_SLOW, 1000));
    CHECK(led_level(LED_FAST, 124) && !led_level(LED_FAST, 125) && led_level(LED_FAST, 250));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("led_pattern_test: ok\n");
    return 0;
}
