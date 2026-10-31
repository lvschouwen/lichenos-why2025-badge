// When a radio change is confirmed first (#33): see radio_confirm.h
#include "radio_confirm.h"

#include <math.h>

int radio_band(float freq_mhz) {
    return freq_mhz < 880.0f ? 0 : 1;
}

radio_confirm_t radio_confirm_needed(float cur_freq, float cur_bw, uint8_t cur_sf, uint8_t cur_cr, uint8_t cur_hash,
                                     float new_freq, float new_bw, uint8_t new_sf, uint8_t new_cr, uint8_t new_hash) {
    if (fabsf(cur_freq - new_freq) < 0.0005f && fabsf(cur_bw - new_bw) < 0.01f && cur_sf == new_sf && cur_cr == new_cr &&
        cur_hash == new_hash) {
        return RADIO_CONFIRM_NONE;
    }
    return radio_band(cur_freq) != radio_band(new_freq) ? RADIO_CONFIRM_BAND : RADIO_CONFIRM_SWITCH;
}
