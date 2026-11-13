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
#define FRF_MAX          1048576000u // 1000 MHz

uint32_t lora_rb_freq_hz(uint32_t frf) {
    if (frf == 0 || frf > FRF_MAX) {
        return 0;
    }
    return (uint32_t)llround((double)frf * 32e6 / (double)(1UL << FRF_DIV_EXPONENT));
}

int16_t lora_rb_signal_rssi_x10(uint32_t packet_status) {
    return (int16_t)(-5 * (int16_t)(packet_status & 0xFF)); // 0.5 dB per unit, negative
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
        .freq_hz      = lora_rb_freq_hz(raw->frf),
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
