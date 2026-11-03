#pragma once

#include "wifi_list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi analyser maths (#159): APs per 2.4 GHz channel, a signal history for one AP, and the scan rate limit. ESP-free.
#define WIFI_AN_CHANNELS 13
#define WIFI_METER_LEN   60
#define WIFI_RSSI_NONE   INT8_MIN // no reading
#define WIFI_AN_SCAN_GAP_MS 10000 // scans disturb the link (#146): never faster than this

typedef struct {
    uint8_t count[WIFI_AN_CHANNELS + 1]; // index = channel 1..13 (0 unused)
    int8_t  best[WIFI_AN_CHANNELS + 1];  // strongest RSSI on the channel, WIFI_RSSI_NONE when empty
} wifi_chan_stats_t;

// Counts every AP (hidden ones too) by primary channel; APs on channel 0 or above 13 are not counted
void wifi_chan_stats(wifi_ap_t const *aps, size_t n, wifi_chan_stats_t *out);

// The last WIFI_METER_LEN readings, a ring; a scan that did not see the AP pushes WIFI_RSSI_NONE
typedef struct {
    int8_t  v[WIFI_METER_LEN];
    uint8_t n;    // readings held
    uint8_t head; // next slot to write
} wifi_meter_t;
void   wifi_meter_clear(wifi_meter_t *m);
void   wifi_meter_push(wifi_meter_t *m, int8_t rssi);
// i = 0 is the oldest reading held, n - 1 the newest; WIFI_RSSI_NONE past the end
int8_t wifi_meter_at(wifi_meter_t const *m, size_t i);

// The strongest AP called ssid in a raw scan; false when it was not seen
bool wifi_an_lookup(wifi_ap_t const *aps, size_t n, char const *ssid, wifi_ap_t *out);
// 0-100 for a bar: -100 dBm or less is 0, -30 dBm or more is 100
int  wifi_level_pct(int8_t rssi);
// A scan is due when none ran yet or WIFI_AN_SCAN_GAP_MS passed (the clock may wrap)
bool wifi_an_scan_due(bool ever, uint32_t last_ms, uint32_t now_ms);
