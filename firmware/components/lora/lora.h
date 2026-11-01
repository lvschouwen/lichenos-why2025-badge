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
// OS only (main.c's update hook, #76, #84 T7): tells the C6 to stop receiving, ends the LoRa task and drops what the C6
// still sends. No start again before the restart. Waits up to about 4 s. An app asks for it by returning true from
// stop_for_update.
void lora_stop(void);
// OS only, the C6 supervisor's hooks (#14 T-B; main.c wires them, no-ops after lora_stop). lost: sends fail at once and
// the task stops polling. back: the task registers its peer-data callback again and re-sends config + RX start (the
// owner's radio settings and TX power are kept here). Both return at once; the LoRa task does the work.
void lora_c6_lost(void);
void lora_c6_back(void);
// OS only (#14 T-B): asks the C6 for its status through the LoRa task and waits up to timeout_ms for the answer. True
// with the C6's uptime when it answered; false while stopped, lost, or when no answer came.
bool lora_c6_probe(uint32_t timeout_ms, uint32_t *uptime_s);
// OS only (#14 T-B): told about every peer-data send to the C6, from the task that sent it (keep it short). Repeated
// failures are the supervisor's soft signal that the C6 may be gone.
void lora_set_send_hook(void (*fn)(bool ok));

// The radio has one owner at a time (#84 T7): the owner's callback gets every received packet (from the LoRa task:
// copy and return, don't block). A second owner is refused and logged until the first releases.
typedef void (*lora_packet_cb_t)(uint8_t const *data, size_t len, int16_t rssi_x10, int16_t snr_x10);
bool lora_claim(char const *owner, lora_packet_cb_t cb);
void lora_release(char const *owner);
// The owner hears when the radio goes away with a C6 reset (false) and when it works again (true) (#14): called from
// the LoRa task, keep it short. Refused (false) unless owner holds the claim; NULL removes it. Released with the claim.
bool lora_set_link_callback(char const *owner, void (*fn)(bool up));
// The owner describes its packets for the LoRa log and "fw lora": false when the bytes are not one of its packets
typedef bool (*lora_rx_formatter_t)(uint8_t const *data, size_t len, char *out, size_t cap);
void lora_set_rx_formatter(lora_rx_formatter_t fmt);
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
// Packets received (every one the C6 handed over) and sent on air (the C6 answered OK); both only go up. Two loads under
// the state lock, cheap enough for the status bar's 100 ms poll (#29).
void lora_get_counts(uint32_t *rx, uint32_t *tx);
