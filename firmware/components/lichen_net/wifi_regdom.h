#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Wi-Fi regulatory domain and the driver read-back for the settings registry (#311). Pure, host-tested
// (wifi_reg_test.c); wifi_service.c sets the country, does the RPCs and keeps one wifi_readback_t, the registry
// (lichen_os reg_wifi.c) maps it to its wifi.* keys.
//
// The country is fixed in firmware (Lucas, 2026-11-10): NL at every Wi-Fi start, 802.11d off (policy manual), so an
// AP's country IE never widens or narrows it. NL allows 2.4 GHz channels 1-13; the driver's default "01" (world safe)
// has 1-11.
#define WIFI_COUNTRY       "NL"
#define WIFI_COUNTRY_SCHAN 1
#define WIFI_COUNTRY_NCHAN 13

// The driver's numbers, pinned against esp_wifi_types_generic.h in wifi_service.c
#define WIFI_RB_POLICY_MANUAL 1 // WIFI_COUNTRY_POLICY_MANUAL
#define WIFI_RB_PS_NONE       0 // WIFI_PS_NONE
#define WIFI_RB_PS_MIN        1 // WIFI_PS_MIN_MODEM
#define WIFI_RB_TX_DEFAULT    80 // 20 dBm in 0.25 dBm units, CONFIG_ESP_PHY_MAX_WIFI_TX_POWER on the C6

// What the C6's Wi-Fi driver applies, read over the esp_hosted RPCs, plus what the P4 asked for. The *_ok flags say
// which reads succeeded the last time; at_ms is when that was.
typedef struct {
    bool     have; // read at least once since boot
    uint32_t at_ms;
    bool     country_ok, proto_ok, bw_ok, tx_ok, ps_ok;
    char     cc[3]; // esp_wifi_get_country: two letters and a third octet (' ', 'O', 'I', 'X' or NUL), no NUL
    uint8_t  schan, nchan, policy;
    int8_t   country_max_dbm;
    uint8_t  proto;  // WIFI_PROTOCOL_* bits
    uint8_t  bw_mhz; // 20, 40 ...; 0 = a bandwidth this build does not name
    int8_t   tx_raw; // 0.25 dBm units
    uint8_t  ps;     // wifi_ps_type_t
    // Requested by the P4: 0 = the driver's own choice (not asked)
    int8_t   req_tx_raw;
    uint8_t  req_bw_mhz, req_proto;
    uint8_t  req_ps;
} wifi_readback_t;

// The driver's three-octet country to text: "NL" (the third octet dropped, anything but a letter or digit as '?');
// bytes written without the NUL, 0 when cap < 3
size_t wifi_country_text(char const cc[3], char *out, size_t cap);
// NL as set: the code, channels 1-13 and policy manual
bool wifi_regdom_expected(wifi_readback_t const *r);
// The log line's body: "NL, channels 1-13, max 20 dBm, policy manual" ("not read" when the read failed)
size_t wifi_regdom_text(wifi_readback_t const *r, char *buf, size_t cap);
// wifi_bandwidth_t to MHz: BW20 -> 20, BW40 -> 40, BW80 -> 80, BW160 and BW80+80 -> 160, anything else 0
uint8_t wifi_bw_to_mhz(int bw);

// The `fw wifi tx|bw|ps` alias replies (#311): the applied value read back, after what was asked
// "wifi: max TX power 13.00 dBm applied (set; not saved)", "... (default; ...)", "... set, applied not readable ..."
size_t wifi_tx_reply(wifi_readback_t const *r, char *buf, size_t cap);
// "wifi: bandwidth 40 MHz asked, 20 MHz applied now (reconnecting to apply, not saved)"; asked_mhz 0 = only the read
size_t wifi_bw_reply(wifi_readback_t const *r, unsigned asked_mhz, bool reconnecting, char *buf, size_t cap);
// "wifi: power save min applied (fw wifi ps none | min, not saved)"
size_t wifi_ps_reply(wifi_readback_t const *r, char *buf, size_t cap);
