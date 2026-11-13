// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// Host tests for lora_tune (#370): every region preset reaches the SX1262 with RadioLib 8 exactly as with 7.8.1.
// The frf pins are the SetRfFrequency steps RadioLib 7.8.1 wrote (computed from its SX126x::setFrequencyRaw); a drift
// of one step fails. The RadioLib-internal mappings this relies on (bandwidth code, low data rate optimisation) are
// replicated from both versions' sources and compared over every value lora_config_valid() lets through.
#include "lora_proto.h"
#include "lora_readback.h"
#include "lora_tune.h"
#include "radio_presets.h"

#include <math.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// What each preset (radio_presets.c, in its order) must reach the radio as: Hz, Hz, SF, CR and frf steps
typedef struct {
    uint32_t freq_hz, bw_hz;
    uint8_t  sf, cr;
    uint32_t frf;
} preset_pin_t;

static preset_pin_t const pins[] = {
    {869618000, 62500, 7, 5, 911860544},   // Netherlands: the badge's current setting
    {869618000, 62500, 8, 8, 911860544},   // Netherlands (Limburg)
    {869618000, 62500, 8, 8, 911860544},   // EU/UK (Narrow)
    {869525000, 250000, 11, 5, 911763008}, // EU/UK (Deprecated)
    {869432000, 62500, 7, 5, 911665536},   // Czech Republic (Narrow)
    {869618000, 62500, 7, 5, 911860544},   // Hungary
    {869618000, 62500, 7, 6, 911860544},   // Portugal 868
    {869618000, 62500, 7, 5, 911860544},   // Slovakia
    {869618000, 62500, 8, 8, 911860544},   // Switzerland
    {910525000, 62500, 7, 5, 954754624},   // USA
    {927875000, 62500, 7, 5, 972947456},   // USA - Southern California
    {910525000, 62500, 7, 5, 954754624},   // Canada
    {910525000, 125000, 11, 5, 954754624}, // Costa Rica
    {923125000, 62500, 8, 8, 967966720},   // Brazil
    {915800000, 250000, 10, 5, 960285888}, // Australia
    {916575000, 62500, 7, 7, 961098496},   // Australia (Narrow)
    {915075000, 125000, 9, 5, 959525696},  // Australia (Mid)
    {923125000, 62500, 8, 8, 967966720},   // Australia: SA, WA
    {923125000, 62500, 8, 5, 967966720},   // Australia: QLD
    {917375000, 62500, 7, 5, 961937408},   // New Zealand (Narrow)
    {917375000, 250000, 11, 5, 961937408}, // New Zealand (Gisborne)
    {921000000, 62500, 7, 5, 965738496},   // Thailand
    {920250000, 62500, 8, 5, 964952064},   // Vietnam (Narrow)
    {920250000, 250000, 11, 5, 964952064}, // Vietnam (Deprecated)
};

#define PIN_COUNT (sizeof(pins) / sizeof(pins[0]))

// RadioLib 8's own Hz -> frf (SX126x::setFrequencyRaw, integer and exact): what the chip would get without tune()
static uint32_t radiolib8_frf(uint32_t hz) {
    return (uint32_t)(((uint64_t)hz << 25) / 32000000u);
}

// The badge's defaults on top of a preset (lora.c: sync 0x12 private, 16 symbols, 22 dBm), as the P4 rounds them
// (LoraRadio::setParams)
static lora_config_t preset_config(radio_preset_t const *p) {
    lora_config_t c = {
        .freq_hz      = (uint32_t)lround(p->freq_mhz * 1000.0) * 1000,
        .bw_hz        = (uint32_t)lround(p->bw_khz * 1000.0),
        .sf           = p->sf,
        .cr           = p->cr,
        .sync_word    = 0x12,
        .preamble     = 16,
        .tx_power_dbm = 22,
    };
    return c;
}

static void test_presets(void) {
    // A preset added to the table needs its pin here
    CHECK(radio_preset_count() == PIN_COUNT);
    unsigned moved = 0;
    for (size_t i = 0; i < PIN_COUNT && i < radio_preset_count(); i++) {
        lora_config_t const c = preset_config(radio_preset_at(i));
        CHECK(lora_config_valid(&c));
        lora_tune_t t;
        lora_tune(&c, &t);
        preset_pin_t const *want = &pins[i];
        CHECK(t.freq_hz == want->freq_hz);
        CHECK(t.bw_hz == want->bw_hz);
        CHECK(t.sf == want->sf && t.cr == want->cr);
        CHECK(t.sync_word == 0x12 && t.preamble == 16 && t.power_dbm == 22);
        CHECK(t.frf == want->frf);
        moved += radiolib8_frf(t.freq_hz) != want->frf;
    }
    // Without tune() the carrier would move: 869.618 MHz by 19 steps, and 15 of the 24 presets (7 distinct
    // frequencies) in all
    CHECK(radiolib8_frf(869618000) == 911860563);
    CHECK(moved == 15);
}

// The steps of the sweep's band edges and of frequencies on whole MHz (exact in float, so both versions agree)
static void test_frf(void) {
    CHECK(lora_tune_frf(862000000) == 903872512);
    CHECK(lora_tune_frf(928000000) == 973078528);
    CHECK(lora_tune_frf(868000000) == radiolib8_frf(868000000));
    // Read back, the steps give the frequency 7.8.1's readback showed: within half a 64-step float grid of the request
    CHECK(lora_rb_freq_hz(lora_tune_frf(869618000)) == 869617981);
    CHECK(lora_rb_freq_hz(lora_tune_frf(928000000)) == 928000000);
}

// SX126x SetModulationParams BW code per bandwidth, RadioLib 7.8.1 (setBandwidth(float kHz): bw / 2 + 0.01 truncated)
static int bw_code_781(uint32_t bw_hz) {
    float const bw = bw_hz / 1e3f; // lora_service's old bw_hz / 1e3f
    switch ((uint8_t)(bw / 2 + 0.01f)) {
        case 3: return 0x00;
        case 5: return 0x08;
        case 7: return 0x01;
        case 10: return 0x09;
        case 15: return 0x02;
        case 20: return 0x0A;
        case 31: return 0x03;
        case 62: return 0x04;
        case 125: return 0x05;
        case 250: return 0x06;
        default: return -1;
    }
}

// ... and RadioLib 8.0.0 (setBandwidth(uint32_t Hz): exact values only)
static int bw_code_800(uint32_t bw_hz) {
    switch (bw_hz) {
        case 7800: return 0x00;
        case 10400: return 0x08;
        case 15600: return 0x01;
        case 20800: return 0x09;
        case 31250: return 0x02;
        case 41700: return 0x0A;
        case 62500: return 0x03;
        case 125000: return 0x04;
        case 250000: return 0x05;
        case 500000: return 0x06;
        default: return -1;
    }
}

// Low data rate optimisation as SX126x::setModulationParams chooses it: 7.8.1 in float ms, 8.0.0 in integer us
static bool ldro_781(uint8_t sf, uint32_t bw_hz) {
    float const bw_khz = bw_hz / 1e3f;
    return (float)(1UL << sf) / bw_khz >= 16.0f;
}

static bool ldro_800(uint8_t sf, uint32_t bw_hz) {
    return (1000000ULL << sf) / bw_hz >= 16000;
}

// Every bandwidth lora_config_valid() accepts gets the same chip code and, with every SF, the same LDRO
static void test_bandwidths(void) {
    static uint32_t const bws[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};
    for (size_t i = 0; i < sizeof(bws) / sizeof(bws[0]); i++) {
        lora_config_t c = {.freq_hz = 869618000, .bw_hz = bws[i], .sf = 7, .cr = 5, .preamble = 16};
        CHECK(lora_config_valid(&c));
        lora_tune_t t;
        lora_tune(&c, &t);
        CHECK(t.bw_hz == bws[i]);
        CHECK(bw_code_800(t.bw_hz) >= 0 && bw_code_800(t.bw_hz) == bw_code_781(bws[i]));
        CHECK(lora_rb_bw_hz((uint8_t)bw_code_800(t.bw_hz)) != 0);
        for (uint8_t sf = 5; sf <= 12; sf++) {
            CHECK(ldro_800(sf, bws[i]) == ldro_781(sf, bws[i]));
        }
    }
}

// The packet RSSI as 7.8.1's getRSSI() * 10 gave it: SignalRssiPkt (the low byte), not RssiPkt (bits 23..16)
static void test_packet_rssi(void) {
    CHECK(lora_rb_signal_rssi_x10(0x000000) == 0);
    CHECK(lora_rb_signal_rssi_x10(0x5A1CC8) == -1000); // 0xC8 = 200 -> -100.0 dBm, whatever RssiPkt says
    CHECK(lora_rb_signal_rssi_x10(0x00FF01) == -5);
    CHECK(lora_rb_signal_rssi_x10(0xFFFFFF) == -1275);
}

int main(void) {
    test_presets();
    test_frf();
    test_bandwidths();
    test_packet_rssi();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_tune_test: all passed\n");
    return 0;
}
