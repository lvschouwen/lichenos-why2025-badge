#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The LoRa radio for apps (#84 T7, #179). The radio has one owner at a time; every call that sends, changes the radio
// or registers a callback takes the owner's name and is refused (false or -1, with a rate-limited log line) unless that
// name holds the claim. Starting, stopping and switching the radio are the OS's (lora_os.h), never an app's.

#define LORA_SUMMARY_MAX 128 // as long as a packet summary line

typedef struct {
    uint32_t packets;
    int16_t  last_rssi_dbx10;
    char     last_summary[LORA_SUMMARY_MAX];
} lora_ui_stats_t;

// The owner's callback gets every received packet (from the LoRa task: copy and return, don't block). A second owner
// is refused and logged until the first releases; claiming again under the same name is fine (a new callback).
typedef void (*lora_packet_cb_t)(uint8_t const *data, size_t len, int16_t rssi_x10, int16_t snr_x10);
bool lora_claim(char const *owner, lora_packet_cb_t cb);
// Gives the radio back with its packet, link and formatter callbacks; a non-owner's release changes nothing
void lora_release(char const *owner);
// The owner hears when the radio goes away with a C6 reset (false) and when it works again (true) (#14): called from
// the LoRa task, keep it short. NULL removes it. Released with the claim.
bool lora_set_link_callback(char const *owner, void (*fn)(bool up));
// The owner describes its packets for the LoRa log and "fw lora": false when the bytes are not one of its packets.
// Released with the claim.
typedef bool (*lora_rx_formatter_t)(uint8_t const *data, size_t len, char *out, size_t cap);
bool lora_set_rx_formatter(char const *owner, lora_rx_formatter_t fmt);
// New frequency, bandwidth, spreading factor and coding rate (from MeshCore's prefs); sent to the C6 within a second.
// false (nothing changed) when lora_config_valid() refuses them. Sync word and preamble stay as lora.toml says.
bool lora_set_radio(char const *owner, uint32_t freq_hz, uint32_t bw_hz, uint8_t sf, uint8_t cr);
// Transmit power for the next packets (-9..22 dBm); false when out of range
bool lora_set_tx_power(char const *owner, int8_t dbm);
// Hands one packet to the C6 (#65); false while the previous one is still on its way, while the radio is off or
// stopped. Non-blocking: poll lora_send_state(), which says 0 (still sending), 1 (sent) or -1 (failed, nothing sent,
// or not the owner) and forgets a finished send.
bool lora_send(char const *owner, uint8_t const *bytes, size_t len, uint32_t airtime_ms);
int  lora_send_state(char const *owner);

// Read-only, for anyone
// The radio task is running: started and not stopped for an update (says nothing about the C6 link)
bool lora_up(void);
// The radio settings in use (from lora.toml or the owner), for airtime estimates
void lora_get_radio(uint8_t *sf, uint32_t *bw_hz, uint8_t *cr, uint16_t *preamble);
// What lora.toml (or the built-in default) gave at boot: MeshCore's first-boot radio settings
void lora_get_boot_radio(uint32_t *freq_hz, uint32_t *bw_hz, uint8_t *sf, uint8_t *cr);
void lora_get_stats(lora_ui_stats_t *out);
// Packets received (every one the C6 handed over) and sent on air (the C6 answered OK); both only go up. Two loads under
// the state lock, cheap enough for the status bar's 100 ms poll (#29).
void lora_get_counts(uint32_t *rx, uint32_t *tx);
// The C6's last channel reading (its status poll, #157), dBm * 10: with no packet being received this is the noise floor.
// False before the first answer or when it is older than max_age_s.
bool lora_get_channel_rssi(int16_t *rssi_x10, uint32_t max_age_s);

#ifdef __cplusplus
}
#endif
