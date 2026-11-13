// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// What lora_service hands RadioLib 8 for a lora_config_t, and the synthesizer steps the SX1262 gets (#370). Pure C, no
// ESP or RadioLib headers: built into the host tests, which pin every region preset, as well as the C6 image.
//
// RadioLib 8 takes frequency and bandwidth as integer Hz. The bandwidths lora_config_valid() accepts are exactly the ten
// RadioLib 8 matches, so they pass unchanged. The frequency does not: RadioLib 7.8.1 turned MHz into the chip's frf
// steps (32 MHz / 2^25, about 0.95 Hz) in single-precision float, which lands on a multiple of 64 steps, and RadioLib 8
// computes the exact step. For 869.618 MHz that is 19 steps (18 Hz) higher. To keep what goes on air unchanged, the
// service writes the 7.8.1 steps itself after every RadioLib frequency change; no whole Hz value handed to RadioLib
// reaches every one of them (one Hz is 1.05 steps).
#pragma once

#include "lora_proto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t freq_hz;   // ConfigLoRa_t::frequency (Hz): the request, for RadioLib's image calibration and bookkeeping
    uint32_t bw_hz;     // ConfigLoRa_t::bandwidth (Hz)
    uint8_t  sf;        // ConfigLoRa_t::spreadingFactor
    uint8_t  cr;        // ConfigLoRa_t::codingRate, the denominator (5..8 = 4/5..4/8)
    uint8_t  sync_word; // ConfigLoRa_t::syncWord
    int8_t   power_dbm; // ConfigLoRa_t::power
    uint16_t preamble;  // ConfigLoRa_t::preambleLength (symbols)
    uint32_t frf;       // the SetRfFrequency steps written after RadioLib's: lora_tune_frf(freq_hz)
} lora_tune_t;

// The SetRfFrequency steps RadioLib 7.8.1 wrote for freq_hz (lora_service's freq_hz / 1e6f, then
// SX126x::setFrequencyRaw's float expression)
uint32_t lora_tune_frf(uint32_t freq_hz);

// lora_config_t (already lora_config_valid) in RadioLib 8's units, plus the steps to write
void lora_tune(lora_config_t const *c, lora_tune_t *out);

#ifdef __cplusplus
}
#endif
