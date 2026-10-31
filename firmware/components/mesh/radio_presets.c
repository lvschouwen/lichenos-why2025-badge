// MeshCore's region presets and the badge's radio limits (#67): see radio_presets.h
#include "radio_presets.h"

#include <math.h>

// MeshCore's app list (api.meshcore.nz suggested_radio_settings, 2026-10-30), in its order, 433 MHz entries left out
static radio_preset_t const presets[] = {
    {"Netherlands", 869.618f, 62.5f, 7, 5, 1},
    {"Netherlands (Limburg)", 869.618f, 62.5f, 8, 8, 2},
    {"EU/UK (Narrow)", 869.618f, 62.5f, 8, 8, 1},
    {"EU/UK (Deprecated)", 869.525f, 250.0f, 11, 5, 1},
    {"Czech Republic (Narrow)", 869.432f, 62.5f, 7, 5, 2},
    {"Hungary", 869.618f, 62.5f, 7, 5, 2},
    {"Portugal 868", 869.618f, 62.5f, 7, 6, 1},
    {"Slovakia", 869.618f, 62.5f, 7, 5, 2},
    {"Switzerland", 869.618f, 62.5f, 8, 8, 1},
    {"USA", 910.525f, 62.5f, 7, 5, 1},
    {"USA - Southern California", 927.875f, 62.5f, 7, 5, 3},
    {"Canada", 910.525f, 62.5f, 7, 5, 3},
    {"Costa Rica", 910.525f, 125.0f, 11, 5, 1},
    {"Brazil", 923.125f, 62.5f, 8, 8, 1},
    {"Australia", 915.800f, 250.0f, 10, 5, 1},
    {"Australia (Narrow)", 916.575f, 62.5f, 7, 7, 1},
    {"Australia (Mid)", 915.075f, 125.0f, 9, 5, 1},
    {"Australia: SA, WA", 923.125f, 62.5f, 8, 8, 1},
    {"Australia: QLD", 923.125f, 62.5f, 8, 5, 1},
    {"New Zealand (Narrow)", 917.375f, 62.5f, 7, 5, 2},
    {"New Zealand (Gisborne)", 917.375f, 250.0f, 11, 5, 1},
    {"Thailand", 921.000f, 62.5f, 7, 5, 1},
    {"Vietnam (Narrow)", 920.250f, 62.5f, 8, 5, 1},
    {"Vietnam (Deprecated)", 920.250f, 250.0f, 11, 5, 1},
};

#define PRESET_COUNT (sizeof(presets) / sizeof(presets[0]))

// The SX1262's LoRa bandwidths, as lora_config_valid() takes them
static float const bandwidths[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f};

size_t radio_preset_count(void) {
    return PRESET_COUNT;
}

radio_preset_t const *radio_preset_at(size_t index) {
    return index < PRESET_COUNT ? &presets[index] : NULL;
}

static bool same_bw(float a, float b) {
    return fabsf(a - b) < 0.01f;
}

int radio_preset_match(float freq, float bw, uint8_t sf, uint8_t cr) {
    for (size_t i = 0; i < PRESET_COUNT; i++) {
        radio_preset_t const *p = &presets[i];
        if (fabsf(p->freq_mhz - freq) <= 0.0005f && same_bw(p->bw_khz, bw) && p->sf == sf && p->cr == cr) {
            return (int)i;
        }
    }
    return -1;
}

int radio_preset_match_exact(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t path_hash_size) {
    for (size_t i = 0; i < PRESET_COUNT; i++) {
        radio_preset_t const *p = &presets[i];
        if (fabsf(p->freq_mhz - freq) <= 0.0005f && same_bw(p->bw_khz, bw) && p->sf == sf && p->cr == cr &&
            p->path_hash_size == path_hash_size) {
            return (int)i;
        }
    }
    return -1;
}

size_t radio_bandwidth_count(void) {
    return sizeof(bandwidths) / sizeof(bandwidths[0]);
}

float radio_bandwidth_at(size_t index) {
    return index < radio_bandwidth_count() ? bandwidths[index] : 0.0f;
}

char const *radio_check(float freq, float bw, uint8_t sf, uint8_t cr, int8_t tx_dbm) {
    if (!(freq >= 862.0f && freq <= 928.0f)) { // NaN too
        return "Frequency must be 862 to 928 MHz";
    }
    bool bw_ok = false;
    for (size_t i = 0; i < radio_bandwidth_count(); i++) {
        bw_ok |= same_bw(bw, bandwidths[i]);
    }
    if (!bw_ok) {
        return "Bandwidth must be 7.8, 10.4, 15.6, 20.8, 31.25, 41.7, 62.5, 125, 250 or 500 kHz";
    }
    if (sf < 5 || sf > 12) {
        return "SF must be 5 to 12";
    }
    if (cr < 5 || cr > 8) {
        return "CR must be 5 to 8 (4/5 to 4/8)";
    }
    if (tx_dbm < -9 || tx_dbm > 22) {
        return "TX power must be -9 to 22 dBm";
    }
    return NULL;
}
