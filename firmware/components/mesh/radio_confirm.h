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

radio_confirm_t radio_confirm_needed(float cur_freq, float cur_bw, uint8_t cur_sf, uint8_t cur_cr, uint8_t cur_hash,
                                     float new_freq, float new_bw, uint8_t new_sf, uint8_t new_cr, uint8_t new_hash);

#ifdef __cplusplus
}
#endif
