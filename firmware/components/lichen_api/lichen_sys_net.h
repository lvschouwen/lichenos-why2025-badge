#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi and mDNS, the shell's side (#244, #216 decision 1): the Wi-Fi screens, the analyser, the status bar's link
// look and Settings go through here, never to lichen_net / lichen_mdns headers. System surface: tools/layers.conf
// forbids lichen_sys_* in L6. The types repeat lichen_net's (the gate cannot include it); lichen_sys_pure_view.c keeps
// them equal with asserts. The pure helpers forward one-for-one to lichen_net's pure code (wifi_list.c, wifi_analyser.c).

#define LICHEN_SYS_WIFI_SSID_MAX      32 // bytes, without the NUL
#define LICHEN_SYS_WIFI_SAVED_MAX     5  // saved networks
#define LICHEN_SYS_WIFI_AN_CHANNELS   13
#define LICHEN_SYS_WIFI_METER_LEN     60
#define LICHEN_SYS_WIFI_RSSI_NONE     INT8_MIN // no reading
#define LICHEN_SYS_WIFI_AN_SCAN_GAP_MS 10000   // scans disturb the link: never faster than this
#define LICHEN_SYS_MDNS_NAME_MAX      16 // "lichen-abcd" plus NUL

typedef struct {
    char    ssid[LICHEN_SYS_WIFI_SSID_MAX + 1];
    int8_t  rssi;
    bool    open;    // no password needed
    uint8_t channel; // 1-13 on 2.4 GHz, 0 when unknown
} lichen_sys_wifi_ap_t;

typedef enum {
    LICHEN_SYS_WIFI_JOIN_NONE,
    LICHEN_SYS_WIFI_JOIN_PENDING,
    LICHEN_SYS_WIFI_JOIN_TRYING,
    LICHEN_SYS_WIFI_JOIN_JOINED,
    LICHEN_SYS_WIFI_JOIN_FAILED_KEPT_OLD,
} lichen_sys_wifi_join_t;

typedef struct {
    uint8_t count[LICHEN_SYS_WIFI_AN_CHANNELS + 1]; // index = channel 1..13
    int8_t  best[LICHEN_SYS_WIFI_AN_CHANNELS + 1];  // strongest RSSI on the channel, RSSI_NONE when empty
} lichen_sys_wifi_chan_stats_t;

// The last LICHEN_SYS_WIFI_METER_LEN readings of one AP, a ring (use the functions below)
typedef struct {
    int8_t  v[LICHEN_SYS_WIFI_METER_LEN];
    uint8_t n;
    uint8_t head;
} lichen_sys_wifi_meter_t;

// The shell's source hooks (shell_sources_t), in the gate's types. main hands them to shell_set_sources. scan_raw keeps
// every AP with its channel; scan the cleaned list. Both block for seconds: never the LVGL task. join_state: the
// outcome of the last wifi join, tried/kept get the SSIDs.
bool lichen_sys_wifi_scan(lichen_sys_wifi_ap_t *out, size_t max, size_t *n);
bool lichen_sys_wifi_scan_raw(lichen_sys_wifi_ap_t *out, size_t max, size_t *n);
lichen_sys_wifi_join_t lichen_sys_wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap);

// The pure helpers below are safe from any task.
// Drops empty SSIDs, keeps the strongest entry per SSID, sorts strongest first; returns the new count.
size_t lichen_sys_wifi_list_clean(lichen_sys_wifi_ap_t *aps, size_t n);
// Copies an SSID with control bytes and invalid UTF-8 as \xNN; always terminated; returns the length.
size_t lichen_sys_wifi_ssid_sanitize(char const *in, char *out, size_t cap);
// Signal bars 0-4 and a bar fill 0-100 for a level
int lichen_sys_wifi_bars(int8_t rssi);
int lichen_sys_wifi_level_pct(int8_t rssi);
// SSID 1-32 bytes, password empty (open network) or 8-63 bytes
bool lichen_sys_wifi_cred_valid(char const *ssid, char const *pass);
// Counts every change of the saved credentials, so a cached SSID knows to read again. Any task.
uint32_t lichen_sys_wifi_cred_generation(void);
// The analyser's maths: APs per channel, a signal history, a lookup and the scan rate limit
void   lichen_sys_wifi_chan_stats(lichen_sys_wifi_ap_t const *aps, size_t n, lichen_sys_wifi_chan_stats_t *out);
void   lichen_sys_wifi_meter_clear(lichen_sys_wifi_meter_t *m);
void   lichen_sys_wifi_meter_push(lichen_sys_wifi_meter_t *m, int8_t rssi);
int8_t lichen_sys_wifi_meter_at(lichen_sys_wifi_meter_t const *m, size_t i); // 0 = oldest held; RSSI_NONE past the end
bool   lichen_sys_wifi_an_lookup(lichen_sys_wifi_ap_t const *aps, size_t n, char const *ssid, lichen_sys_wifi_ap_t *out);
bool   lichen_sys_wifi_an_scan_due(bool ever, uint32_t last_ms, uint32_t now_ms);

// "lichen-xxxx" (no ".local"), from the MAC; valid from boot. Any task.
char const *lichen_sys_mdns_name(void);
