// When a radio change is confirmed first (#33): see radio_confirm.h
#include "radio_confirm.h"

#include <math.h>

int radio_band(float freq_mhz) {
    return freq_mhz < 880.0f ? 0 : 1;
}

radio_confirm_t radio_confirm_needed(radio_confirm_cfg_t const *cur, radio_confirm_cfg_t const *next) {
    if (fabsf(cur->freq - next->freq) < 0.0005f && fabsf(cur->bw - next->bw) < 0.01f && cur->sf == next->sf &&
        cur->cr == next->cr && cur->hash == next->hash) {
        return RADIO_CONFIRM_NONE;
    }
    return radio_band(cur->freq) != radio_band(next->freq) ? RADIO_CONFIRM_BAND : RADIO_CONFIRM_SWITCH;
}
