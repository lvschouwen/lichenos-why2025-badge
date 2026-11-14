// Host test: LoRa airtime and MeshCore's packet score (#61)
#include "lora_airtime.h"

#include <math.h>
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
    // Semtech's worked example: SF7, 125 kHz, CR 4/5, 8 preamble symbols, 10 bytes -> 41.216 ms
    uint32_t us = lora_airtime_us(10, 7, 125000, 5, 8);
    CHECK(us >= 41200 && us <= 41230);
    // Half the bandwidth, twice the symbol time
    uint32_t half = lora_airtime_us(10, 7, 62500, 5, 8);
    CHECK(half >= 2 * 41200 && half <= 2 * 41230);
    // The badge's mesh preset with a full packet stays well under a second
    CHECK(lora_airtime_us(255, 7, 62500, 5, 16) < 1000000);

    // #452: the extremes saturate instead of an undefined cast. 65535 preamble symbols at SF12/7.8 kHz is about
    // 9.6 h, over UINT32_MAX us; UBSan (float-cast-overflow) caught the old cast here
    CHECK(lora_airtime_us(255, 12, 7800, 8, 65535) == UINT32_MAX);
    CHECK(lora_airtime_us(255, 12, 7800, 8, 64) > 250000000u && lora_airtime_us(255, 12, 7800, 8, 64) < 260000000u);
    CHECK(lora_airtime_us(0, 5, 500000, 5, 6) > 0);
    CHECK(lora_airtime_us(-1000, 7, 62500, 5, 16) > 0); // a negative length counts no payload symbols
    // Never a real config: the longest answer, no division by zero or shift past the width
    CHECK(lora_airtime_us(10, 7, 0, 5, 16) == UINT32_MAX);
    CHECK(lora_airtime_us(10, 13, 125000, 5, 16) == UINT32_MAX);
    CHECK(lora_airtime_us(10, 4, 125000, 5, 16) == UINT32_MAX);
    CHECK(lora_airtime_us(10, 255, 125000, 5, 16) == UINT32_MAX);

    // Score: nothing below the SF7 floor of -7.5 dB, full marks 10 dB above it for an empty packet
    CHECK(lora_packet_score(-8.0f, 7, 20) == 0.0f);
    CHECK(fabsf(lora_packet_score(2.5f, 7, 0) - 1.0f) < 1e-6f);
    CHECK(fabsf(lora_packet_score(2.5f, 7, 128) - 0.5f) < 1e-6f);
    CHECK(lora_packet_score(30.0f, 7, 0) == 1.0f); // clamped
    CHECK(lora_packet_score(5.0f, 6, 10) == 0.0f);  // below SF7 MeshCore gives up

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_airtime_test: all checks passed\n");
    return 0;
}
