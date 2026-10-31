#include "lora_airtime.h"

#include <math.h>

uint32_t lora_airtime_us(int len, uint8_t sf, uint32_t bw_hz, uint8_t cr, uint16_t preamble) {
    double tsym_us = (double)(1u << sf) * 1e6 / (double)bw_hz;
    int    de      = tsym_us >= 16000.0 ? 1 : 0; // low data rate optimisation
    int    crc     = 1, ih = 0;                  // CRC on, explicit header
    double num     = 8.0 * len - 4.0 * sf + 28 + 16 * crc - 20 * ih;
    double symbols = ceil(num / (4.0 * (sf - 2 * de))) * cr; // Semtech's (CR + 4) with CR = cr - 4
    if (symbols < 0) {
        symbols = 0;
    }
    double payload_symbols = 8 + symbols;
    double preamble_us     = (preamble + 4.25) * tsym_us;
    return (uint32_t)(preamble_us + payload_symbols * tsym_us);
}

// RadioLibWrapper's table: the SNR each SF needs at least (SF7 first)
static float const snr_floor[] = {-7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f};

float lora_packet_score(float snr, uint8_t sf, int len) {
    if (sf < 7 || sf > 12 || snr < snr_floor[sf - 7]) {
        return 0.0f;
    }
    float success   = (snr - snr_floor[sf - 7]) / 10.0f;
    float collision = 1.0f - (float)len / 256.0f; // assuming a 256-byte maximum packet
    float score     = success * collision;
    return score < 0.0f ? 0.0f : score > 1.0f ? 1.0f : score;
}
