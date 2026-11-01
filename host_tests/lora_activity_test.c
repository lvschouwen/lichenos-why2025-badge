// Host test: the status bar's LoRa TX/RX chevron fade (#29)
#include "lora_activity.h"

#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void priming_does_not_light(void) {
    lora_act_t a = {0};
    CHECK(lora_act_step(&a, 42, 1000) == 0); // 42 packets already there at boot
    CHECK(lora_act_step(&a, 42, 1100) == 0);
}

static void hold_then_fade(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 0, 0);
    CHECK(lora_act_step(&a, 1, 100) == 255);
    CHECK(lora_act_step(&a, 1, 300) == 255);
    CHECK(lora_act_step(&a, 1, 600) == 255); // 500 ms after the count
    uint8_t last = 255;
    for (uint32_t t = 700; t < 1600; t += 100) {
        uint8_t level = lora_act_step(&a, 1, t);
        CHECK(level < last && level > 0); // falls on every tick of the fade
        last = level;
    }
    CHECK(lora_act_step(&a, 1, 1600) == 0); // 1500 ms after the count
    CHECK(lora_act_step(&a, 1, 5000) == 0);
}

static void relight_mid_fade(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 7, 0);
    lora_act_step(&a, 8, 100);
    CHECK(lora_act_step(&a, 8, 1100) < 255);
    CHECK(lora_act_step(&a, 9, 1200) == 255);
    CHECK(lora_act_step(&a, 9, 1700) == 255);
    CHECK(lora_act_step(&a, 9, 2700) == 0);
}

static void burst_lights_once(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 10, 0);
    CHECK(lora_act_step(&a, 15, 100) == 255); // five packets between two ticks
    CHECK(lora_act_step(&a, 15, 1600) == 0);  // one blink, not five
}

static void count_wrap_lights(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 0xFFFFFFFFu, 0);
    CHECK(lora_act_step(&a, 0, 100) == 255);
}

static void clock_wrap(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 3, 0xFFFFFF00u);
    CHECK(lora_act_step(&a, 4, 0xFFFFFFF0u) == 255);
    CHECK(lora_act_step(&a, 4, 0x100u) == 255); // 272 ms later, past the wrap
    uint8_t mid = lora_act_step(&a, 4, 0x300u); // 784 ms: fading
    CHECK(mid > 0 && mid < 255);
    CHECK(lora_act_step(&a, 4, 0x600u) == 0); // 1552 ms: faint
}

static void unchanged_stays_faint(void) {
    lora_act_t a = {0};
    lora_act_step(&a, 5, 0);
    for (uint32_t t = 100; t < 3000; t += 100) {
        CHECK(lora_act_step(&a, 5, t) == 0);
    }
}

int main(void) {
    priming_does_not_light();
    hold_then_fade();
    relight_mid_fade();
    burst_lights_once();
    count_wrap_lights();
    clock_wrap();
    unchanged_stays_faint();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_activity_test ok\n");
    return 0;
}
