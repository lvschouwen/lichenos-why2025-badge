// Host test (#452): every MeshCore preset the badge offers stays inside the radio bounds both chips share, at every
// preamble lora_config_valid takes, with a full 255-byte frame under the C6's airtime ceiling; and a slow custom
// config the ceiling does refuse, so the ceiling is a real limit
#include "lora_airtime.h"
#include "lora_proto.h"
#include "radio_presets.h"

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

#define FULL_FRAME 255 // MeshCore's MAX_TRANS_UNIT

static lora_config_t preset_config(radio_preset_t const *p, uint16_t preamble) {
    lora_config_t c = LORA_CONFIG_DEFAULT;
    c.freq_hz       = (uint32_t)lround(p->freq_mhz * 1e6);
    c.bw_hz         = (uint32_t)lround(p->bw_khz * 1e3); // as MeshCore's settings round them (lora_radio.cpp)
    c.sf       = p->sf;
    c.cr       = p->cr;
    c.preamble = preamble;
    return c;
}

static void presets_inside_the_bounds(void) {
    uint16_t const preambles[] = {LORA_PREAMBLE_MIN, 16, LORA_PREAMBLE_MAX};
    uint32_t       longest     = 0;
    for (size_t i = 0; i < radio_preset_count(); i++) {
        radio_preset_t const *p = radio_preset_at(i);
        for (size_t j = 0; j < sizeof(preambles) / sizeof(preambles[0]); j++) {
            lora_config_t const c  = preset_config(p, preambles[j]);
            uint32_t const      us = lora_airtime_us(FULL_FRAME, c.sf, c.bw_hz, c.cr, c.preamble);
            printf("%-26s %7.3f MHz %6.2f kHz SF%-2u CR4/%u preamble %2u: %4lu ms\n", p->title, p->freq_mhz, p->bw_khz,
                   p->sf, p->cr, c.preamble, (unsigned long)(us / 1000));
            if (!lora_config_valid(&c) || us > LORA_TX_MAX_AIRTIME_US) {
                printf("FAIL preset %s preamble %u: valid %d, %lu us\n", p->title, c.preamble, lora_config_valid(&c),
                       (unsigned long)us);
                failures++;
            }
            int64_t deadline;
            CHECK(lora_tx_admit(0, us, &deadline));
            longest = us > longest ? us : longest;
        }
    }
    // The longest preset frame (Costa Rica, SF11/125 kHz, at preamble 64) with a margin under the ceiling for the
    // difference between this estimate and RadioLib's on the C6
    CHECK(longest > 5800000u && longest < 6000000u);
    CHECK(longest + 1000000u < LORA_TX_MAX_AIRTIME_US);
}

// The default config is MeshCore's own, and a slow custom one passes lora_config_valid but its full frame does not
// pass the ceiling (the C6 refuses that SEND, short ones still go)
static void ceiling_is_a_real_limit(void) {
    lora_config_t const def = LORA_CONFIG_DEFAULT;
    CHECK(lora_config_valid(&def) && def.preamble == 16);
    lora_config_t slow = def;
    slow.bw_hz         = 125000;
    slow.sf            = 12;
    slow.cr            = 8;
    CHECK(lora_config_valid(&slow));
    int64_t deadline;
    CHECK(!lora_tx_admit(0, lora_airtime_us(FULL_FRAME, slow.sf, slow.bw_hz, slow.cr, slow.preamble), &deadline));
    CHECK(lora_tx_admit(0, lora_airtime_us(20, slow.sf, slow.bw_hz, slow.cr, slow.preamble), &deadline));
}

int main(void) {
    presets_inside_the_bounds();
    ceiling_is_a_real_limit();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_bounds_test: all passed\n");
    return 0;
}
