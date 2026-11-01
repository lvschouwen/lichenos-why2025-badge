// Host test: status bar link looks (#66, Task 9d)
#include "link_state.h"

#include <stddef.h>
#include <stdio.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define TEAL  0x3FC1B0
#define AMBER 0xF5B700
#define RED   0xE5484D
#define DIM   0x8090A0

int main(void) {
    // The look table (#105): off by choice dim, faults red, changes amber, up teal
    static struct {
        bool    creds, joining, ip, known;
        int8_t  rssi;
        link_t  state;
        int     bars;
        uint32_t color;
    } const wifi[] = {
        {true, false, true, true, -50, LINK_UP, 4, TEAL},
        {true, false, true, true, -70, LINK_UP, 2, TEAL},
        {true, false, true, false, 0, LINK_UP, 1, TEAL},  // no reading yet: one bar, not down
        {true, false, true, true, -95, LINK_UP, 0, TEAL},
        {true, true, false, false, 0, LINK_CONNECTING, 0, AMBER},
        {true, false, false, false, 0, LINK_DOWN, 0, RED}, // a fault: credentials but no link
        {false, false, false, false, 0, LINK_OFF, 0, DIM}, // off by choice: a faint fan
    };
    for (size_t i = 0; i < sizeof(wifi) / sizeof(wifi[0]); i++) {
        link_look_t l = wifi_look(wifi[i].creds, wifi[i].joining, wifi[i].ip, wifi[i].known, wifi[i].rssi);
        if (l.state != wifi[i].state || l.bars != wifi[i].bars || l.color != wifi[i].color) {
            printf("FAIL wifi row %u\n", (unsigned)i);
            failures++;
        }
    }
    static struct {
        bool       up;
        lora_use_t use;
        link_t     state;
        uint32_t   color;
    } const lora[] = {
        {true, LORA_USE_RUNNING, LINK_UP, TEAL},
        {false, LORA_USE_RUNNING, LINK_DOWN, RED},      // the radio or the C6 down
        {true, LORA_USE_OFF, LINK_OFF, DIM},            // MeshCore stopped by choice (#103): dim, never red
        {false, LORA_USE_OFF, LINK_OFF, DIM},
        {true, LORA_USE_CHANGING, LINK_CONNECTING, AMBER},
        {false, LORA_USE_CHANGING, LINK_DOWN, RED},
        {true, LORA_USE_FAILED, LINK_DOWN, RED},        // wanted but not running
        {false, LORA_USE_FAILED, LINK_DOWN, RED},
    };
    for (size_t i = 0; i < sizeof(lora) / sizeof(lora[0]); i++) {
        link_look_t l = lora_look(lora[i].up, lora[i].use);
        if (l.state != lora[i].state || l.bars != 0 || l.color != lora[i].color) {
            printf("FAIL lora row %u\n", (unsigned)i);
            failures++;
        }
    }

    // The fan (#58): which of dot, inner, middle, outer arc are lit, for 0..4 bars
    static bool const fan[5][WIFI_FAN_PARTS] = {
        {false, false, false, false}, {true, false, false, false}, {true, true, false, false},
        {true, true, true, false},    {true, true, true, true},
    };
    for (int bars = 0; bars <= 4; bars++) {
        for (int part = 0; part < WIFI_FAN_PARTS; part++) {
            CHECK(wifi_fan_part_lit(bars, part) == fan[bars][part]);
        }
    }
    CHECK(!wifi_fan_part_lit(4, -1) && !wifi_fan_part_lit(9, WIFI_FAN_PARTS));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("link_state_test ok\n");
    return 0;
}
