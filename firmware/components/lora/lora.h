#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// LoRa receive over the C6 bridge (#51, #61): logs every packet, "fw lora" shows the status.

#define LORA_SUMMARY_MAX 128 // as long as a packet summary line

typedef struct {
    uint32_t packets;
    int16_t  last_rssi_dbx10;
    char     last_summary[LORA_SUMMARY_MAX];
} lora_ui_stats_t;

bool lora_start(void);
// The radio task is running: started and not stopped for an update (says nothing about the C6 link)
bool lora_up(void);
// A firmware update is coming (#76): tells the C6 to stop receiving, ends the LoRa task and drops what the C6 still
// sends. No start again before the restart. Waits up to about 4 s.
void lora_stop(void);

// Every received packet is also handed to this callback (from the LoRa task: copy and return, don't block)
typedef void (*lora_packet_cb_t)(uint8_t const *data, size_t len, int16_t rssi_x10, int16_t snr_x10);
void lora_set_packet_callback(lora_packet_cb_t cb);
// The radio settings in use (from lora.toml), for airtime estimates
void lora_get_radio(uint8_t *sf, uint32_t *bw_hz, uint8_t *cr, uint16_t *preamble);
// New frequency, bandwidth, spreading factor and coding rate (from MeshCore's prefs); sent to the C6 within a second.
// false (nothing changed) when lora_config_valid() refuses them. Sync word and preamble stay as lora.toml says.
bool lora_set_radio(uint32_t freq_hz, uint32_t bw_hz, uint8_t sf, uint8_t cr);
// Transmit power for the next packets (-9..22 dBm); false when out of range
bool lora_set_tx_power(int8_t dbm);
// Hands one packet to the C6 (#65); false while the previous one is still on its way. Non-blocking: poll
// lora_send_state(), which says 0 (still sending), 1 (sent) or -1 (failed or nothing sent) and forgets a finished send.
bool lora_send(uint8_t const *bytes, size_t len, uint32_t airtime_ms);
int  lora_send_state(void);
// What lora.toml (or the built-in default) gave at boot: MeshCore's first-boot radio settings
void lora_get_boot_radio(uint32_t *freq_hz, uint32_t *bw_hz, uint8_t *sf, uint8_t *cr);
void lora_get_stats(lora_ui_stats_t *out);
