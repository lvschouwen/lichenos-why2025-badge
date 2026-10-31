#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi scan results and saved credentials (#66), ESP-free except for the NVS calls (host-tested with a stub)

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   open; // no password needed
} wifi_ap_t;

// Drops empty SSIDs, keeps the strongest entry per SSID, sorts by RSSI (strongest first); returns the new count
size_t wifi_list_clean(wifi_ap_t *aps, size_t n);
// Copies an SSID for a console line or a label with control bytes (< 0x20, 0x7F) and invalid UTF-8 as \xNN; valid UTF-8
// stays. Always terminated, never ends in half an escape; returns the length.
size_t wifi_ssid_sanitize(char const *in, char *out, size_t cap);
// Signal bars 0-4: >= -55 -> 4, >= -65 -> 3, >= -75 -> 2, >= -85 -> 1
int    wifi_bars(int8_t rssi);

// SSID 1-32 bytes, password empty (open network) or 8-63 bytes
bool wifi_cred_valid(char const *ssid, char const *pass);

// The credentials in NVS badge_fw wifi_ssid / wifi_pass. The password is never logged. Save and clear count a change
// (wifi_cred_generation), so a cached SSID knows to reread.
bool     wifi_cred_save(char const *ssid, char const *pass);
bool     wifi_cred_load(char ssid[33], char pass[64]);
bool     wifi_cred_clear(void);
uint32_t wifi_cred_generation(void);
