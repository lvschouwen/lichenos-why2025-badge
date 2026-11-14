#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Does switching the radio need a confirm page, and how strong a warning (#33)? Pure logic, no ESP headers (host-tested
// in host_tests/radio_confirm_test.c).
typedef enum {
    RADIO_CONFIRM_NONE,   // the same radio: nothing changes, nothing to ask
    RADIO_CONFIRM_SWITCH, // another radio in the same band
    RADIO_CONFIRM_BAND,   // another ISM band: may not be legal where the badge is
} radio_confirm_t;

// 0 = 862-879.999 MHz (EU 868), 1 = 880 MHz and up (the 902-928 MHz bands of the Americas and Asia-Pacific)
int radio_band(float freq_mhz);

// One radio setup as the confirm check compares it
typedef struct {
    float   freq;
    float   bw;
    uint8_t sf;
    uint8_t cr;
    uint8_t hash;
} radio_confirm_cfg_t;

radio_confirm_t radio_confirm_needed(radio_confirm_cfg_t const *cur, radio_confirm_cfg_t const *next);

#ifdef __cplusplus
}
#endif
