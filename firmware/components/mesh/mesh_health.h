#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The radio health page (#157): MeshCore's own counters (upstream CMD_GET_STATS: core, radio, packets) and the C6's
// channel reading, copied in one pass under the mesh lock, and the texts for them. Receive only: reading these sends
// nothing. Pure logic, no ESP headers: built into the host tests.

typedef struct {
    bool     have_noise;    // the C6 reported a channel level recently
    int16_t  noise_x10;     // dBm * 10, the channel level with no packet being received (the noise floor)
    bool     have_last;     // a packet has been received since start
    int16_t  last_rssi_x10; // dBm * 10 of the last packet
    int16_t  last_snr_x10;  // dB * 10 of the last packet
    uint32_t tx_air_ms, rx_air_ms;
    uint32_t uptime_s;
    uint32_t recv_flood, recv_direct, sent_flood, sent_direct;
    uint32_t recv_errors;  // packets the radio reported but MeshCore could not use
    uint32_t rx_dropped;   // arrived while the receive queue was full
    uint32_t send_failed;  // packets the C6 did not get out
    uint16_t err_flags;    // MeshCore's ERR_EVENT_* bits
    uint8_t  queue_len;    // packets waiting to be sent
} mesh_health_t;

// MeshCore's ERR_EVENT_* (src/Dispatcher.h)
#define MH_ERR_FULL     (1u << 0)
#define MH_ERR_CAD      (1u << 1)
#define MH_ERR_STARTRX  (1u << 2)

// "-87 dBm" (rounded), "6.5 dB" / "-3.0 dB" (one decimal)
void mh_fmt_dbm(int16_t x10, char *out, size_t cap);
void mh_fmt_db(int16_t x10, char *out, size_t cap);
// Airtime with its share of the uptime: "480 ms (0.1%)", "4.2 s (0.4%)", "75 s (2.1%)", "3 min (2.1%)"; "0 s (0%)" for none
void mh_fmt_air(uint32_t air_ms, uint32_t uptime_s, char *out, size_t cap);
// "42 s", "3 min", "2 h 5 min", "1 d 3 h"
void mh_fmt_uptime(uint32_t s, char *out, size_t cap);
// "none" or the events since start, in words: "queue full, rx timeout"
void mh_fmt_errors(uint16_t flags, char *out, size_t cap);
// "12 flood, 3 direct"
void mh_fmt_split(uint32_t flood, uint32_t direct, char *out, size_t cap);
// "2 dropped, 1 failed send, 4 bad" or "none": the three loss counters in one line
void mh_fmt_drops(mesh_health_t const *h, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
