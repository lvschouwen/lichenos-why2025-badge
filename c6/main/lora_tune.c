// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "lora_tune.h"

#define FRF_DIV_EXPONENT 25
#define FRF_CRYSTAL_MHZ  32.0f

uint32_t lora_tune_frf(uint32_t freq_hz) {
    // Both roundings of 7.8.1 kept as they were: the uint32 -> float and the division to MHz. Scaling by 2^25 and
    // dividing by 32 are exact in float, so nothing else rounds.
    float const mhz = (float)freq_hz / 1e6f;
    return (uint32_t)((mhz * (float)(1UL << FRF_DIV_EXPONENT)) / FRF_CRYSTAL_MHZ);
}

void lora_tune(lora_config_t const *c, lora_tune_t *out) {
    *out = (lora_tune_t){
        .freq_hz   = c->freq_hz,
        .bw_hz     = c->bw_hz,
        .sf        = c->sf,
        .cr        = c->cr,
        .sync_word = c->sync_word,
        .power_dbm = c->tx_power_dbm,
        .preamble  = c->preamble,
        .frf       = lora_tune_frf(c->freq_hz),
    };
}
