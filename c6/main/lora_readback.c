// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "lora_readback.h"

#include <math.h>
#include <string.h>

// SX126x datasheet, SetModulationParams (LoRa): the BW codes are not in bandwidth order
static struct {
    uint8_t  code;
    uint32_t hz;
} const bw_codes[] = {
    {0x00, 7812},  {0x08, 10417}, {0x01, 15625},  {0x09, 20833},  {0x02, 31250},
    {0x0A, 41667}, {0x03, 62500}, {0x04, 125000}, {0x05, 250000}, {0x06, 500000},
};

uint32_t lora_rb_bw_hz(uint8_t code) {
    for (unsigned i = 0; i < sizeof(bw_codes) / sizeof(bw_codes[0]); i++) {
        if (bw_codes[i].code == code) {
            return bw_codes[i].hz;
        }
    }
    return 0;
}

uint8_t lora_rb_sync(uint8_t msb, uint8_t lsb) {
    return (uint8_t)((msb & 0xF0) | (lsb >> 4));
}

#define FRF_DIV_EXPONENT 25
#define FRF_CRYSTAL_MHZ  32.0f

uint32_t lora_rb_freq_hz(float mhz) {
    if (!(mhz > 0.0f) || mhz > 1000.0f) {
        return 0;
    }
    // The same float expression as RadioLib's SX126x::setFrequencyRaw, so the step count is the one the chip got
    uint32_t const frf = (uint32_t)((mhz * (float)(1UL << FRF_DIV_EXPONENT)) / FRF_CRYSTAL_MHZ);
    return (uint32_t)llround((double)frf * 32e6 / (double)(1UL << FRF_DIV_EXPONENT));
}

uint8_t lora_rb_cr(uint8_t code) {
    return code >= 1 && code <= 4 ? (uint8_t)(code + 4) : 0;
}

bool lora_rb_decode(lora_rb_raw_t const *raw, lora_config_t *out) {
    memset(out, 0, sizeof(*out));
    if (!raw->started || !raw->regs_read) {
        return false;
    }
    lora_config_t const c = {
        .freq_hz      = lora_rb_freq_hz(raw->freq_mhz),
        .bw_hz        = lora_rb_bw_hz(raw->bw_code),
        .sf           = raw->sf,
        .cr           = lora_rb_cr(raw->cr_code),
        .sync_word    = lora_rb_sync(raw->sync_msb, raw->sync_lsb),
        .preamble     = raw->preamble,
        .tx_power_dbm = raw->power_dbm,
    };
    if (!c.freq_hz || !c.bw_hz || !c.cr) {
        return false;
    }
    *out = c;
    return true;
}
