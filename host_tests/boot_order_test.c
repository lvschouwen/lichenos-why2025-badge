// Host test: app_main's start order (#210), the two-chip rules of docs/architecture.md "Start order (main.c)" (#234)
#include "boot_order.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// a runs before b; both must be in the table
static void before(boot_start_t a, boot_start_t b) {
    int ia = boot_order_index(a);
    int ib = boot_order_index(b);
    if (ia < 0 || ib < 0 || ia >= ib) {
        printf("FAIL: %s (at %d) must start before %s (at %d)\n", boot_start_name(a), ia, boot_start_name(b), ib);
        failures++;
    }
}

// main.c walks the table: every step exactly once, nothing out of range
static void each_step_once(void) {
    CHECK(boot_order_len == START_COUNT);
    for (int s = 0; s < START_COUNT; s++) {
        int seen = 0;
        for (size_t i = 0; i < boot_order_len; i++) {
            seen += boot_order[i] == (boot_start_t)s;
        }
        if (seen != 1) {
            printf("FAIL: %s is in the table %d times\n", boot_start_name((boot_start_t)s), seen);
            failures++;
        }
    }
}

static void two_chip_rules(void) {
    // Rule 1: the C6 image files are on /storage; esp_hosted adds SDMMC slot 1 to the controller the card shares
    before(START_STORAGE, START_C6LINK);
    before(START_SD, START_C6LINK);
    // Rule 2: c6link asks the C6 app through lora_ident, set before the link starts
    before(START_LORA_IDENT, START_C6LINK);
    // Rule 3: the C6 drives the backlights, resets the TCA8418 and carries LoRa
    before(START_C6LINK, START_BACKLIGHT);
    before(START_C6LINK, START_KEYBOARD);
    before(START_C6LINK, START_LORA);
    // ... and the reset recovery hooks only once all three are up
    before(START_BACKLIGHT, START_C6_HOOKS);
    before(START_KEYBOARD, START_C6_HOOKS);
    before(START_LORA, START_C6_HOOKS);
}

static void os_rules(void) {
    before(START_LOGS, START_NVS);                 // the log store and boot progress catch everything after them
    before(START_NVS, START_EARLY);                // haptics and the time zone read settings
    before(START_EARLY, START_BOOT_SCREEN);        // the boot screen reads crash_recovered() once
    before(START_BOOT_SCREEN, START_STORAGE);      // the first frame comes before anything slow (#39)
    before(START_STORAGE, START_NOTICES);          // the notification history is on /storage
    before(START_RADIO_SWITCHES, START_C6LINK);    // the saved switches before Wi-Fi and LoRa start
    before(START_KEYBOARD, START_SENSORS);         // the sensors share the keyboard's bus
    before(START_C6_HOOKS, START_APPS);
}

int main(void) {
    each_step_once();
    two_chip_rules();
    os_rules();
    CHECK(boot_order_index(START_COUNT) == -1);
    CHECK(boot_start_name(START_COUNT)[0] == '?');

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("boot_order_test ok\n");
    return 0;
}
