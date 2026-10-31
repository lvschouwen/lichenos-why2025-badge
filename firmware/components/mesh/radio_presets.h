#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore's region presets and the radio limits of the badge (#67). Pure logic, no ESP headers (host-tested in
// host_tests/radio_presets_test.c). The table is MeshCore's own app list (api.meshcore.nz suggested_radio_settings,
// fetched 2026-10-30) without the 433 MHz entries the SX1262 module can't do, Netherlands first as the default.

typedef struct {
    char const *title;
    float       freq_mhz;
    float       bw_khz;
    uint8_t     sf;
    uint8_t     cr;             // 5..8 for 4/5..4/8
    uint8_t     path_hash_size; // bytes per hop in a flood's path (MeshCore's path_hash_mode + 1)
} radio_preset_t;

size_t                radio_preset_count(void);
radio_preset_t const *radio_preset_at(size_t index); // NULL past the end
// The first preset with these values (frequency within 0.0005 MHz), -1 for none (Custom)
int radio_preset_match(float freq, float bw, uint8_t sf, uint8_t cr);
// The preset in use, path hash size included: the first entry with the same radio and the same size; -1 (Custom) when
// none has both, even if one has the same radio with another size (Netherlands, Hungary and Slovakia differ only there)
int radio_preset_match_exact(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t path_hash_size);
// The SX1262's LoRa bandwidths in kHz, smallest first (radio_check takes exactly these)
size_t radio_bandwidth_count(void);
float  radio_bandwidth_at(size_t index); // 0 past the end
// NULL when the badge can use these values, else the reason, short enough for the screen: 862-928 MHz, the SX1262's
// bandwidths (7.8 ... 500 kHz), SF 5-12, CR 5-8, TX -9...22 dBm
char const *radio_check(float freq, float bw, uint8_t sf, uint8_t cr, int8_t tx_dbm);

#ifdef __cplusplus
}
#endif
