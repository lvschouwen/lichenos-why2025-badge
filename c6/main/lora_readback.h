// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// What the SX1262 runs, decoded from RadioLib's driver state and the chip's registers into the units of lora_config_t
// (#309). Pure C, no ESP or RadioLib headers: the constants are the SX126x datasheet's, pinned by the host test.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The LoRa bandwidth in Hz for an SX126x SetModulationParams BW code (0x00 = 7.8 kHz ... 0x06 = 500 kHz); 0 = unknown
uint32_t lora_rb_bw_hz(uint8_t code);

// The sync word from the two LoRa sync registers (0x0740/0x0741): RadioLib writes its high nibble into the top of the
// first and its low nibble into the top of the second, with control bits below
uint8_t lora_rb_sync(uint8_t msb, uint8_t lsb);

// The frequency the synthesizer runs for RadioLib's frequency in MHz: RadioLib turns it into the chip's frf steps
// (32 MHz / 2^25, about 0.95 Hz) in single-precision float, so the result is within about 64 Hz of the request
uint32_t lora_rb_freq_hz(float mhz);

// The coding rate denominator (5..8 = 4/5..4/8) for RadioLib's stored code (cr - 4 without long interleaving); 0 =
// unknown
uint8_t lora_rb_cr(uint8_t code);

#ifdef __cplusplus
}
#endif
