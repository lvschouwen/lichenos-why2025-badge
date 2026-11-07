#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi scan results and saved credentials (#66), ESP-free: the credentials go through a bound store (host-tested)

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   open; // no password needed
    uint8_t channel; // 1-13 on 2.4 GHz, 0 when unknown (#159)
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

// Where the credentials live (#235): lichen_net opens no NVS, the settings service binds its calls at boot (lichen_os
// settings_bind_net). The SSIDs and wifi_drv_wiped are plain settings, the passwords the secrets class. Unbound, every
// read finds nothing and every write fails.
typedef struct {
    void    (*get_str)(char const *key, char *out, size_t cap, char const *def);
    bool    (*set_str)(char const *key, char const *value);
    bool    (*erase)(char const *key); // true when the key is gone, also when it was never there
    uint8_t (*get_u8)(char const *key, uint8_t def);
    bool    (*set_u8)(char const *key, uint8_t value);
    bool    (*secret_get)(char const *key, char *out, size_t cap); // false when missing; "" is a value
    bool    (*secret_set)(char const *key, char const *value);
} wifi_store_t;
// Once at boot, before the Wi-Fi service starts (the pointer is read by the c6wifi task, the console and the UI)
void wifi_list_bind_store(wifi_store_t const *store);

// The pair in use: badge_fw wifi_ssid (plain) / wifi_pass (secret). The password is never logged. Save and clear count
// a change (wifi_cred_generation), so a cached SSID knows to reread.
bool     wifi_cred_save(char const *ssid, char const *pass);
bool     wifi_cred_load(char ssid[33], char pass[64]);
bool     wifi_cred_clear(void);
uint32_t wifi_cred_generation(void);
// badge_fw wifi_drv_wiped (a plain flag): the C6 driver's own flash copy was wiped once (esp_wifi_restore, after the
// move of the credentials to badge_fw). True when it was, and also when no store is bound (then nothing is wiped).
bool wifi_driver_wiped(void);
void wifi_driver_wiped_set(void);

// The saved networks (#160), keys wifi_s0..wifi_s4 (plain) / wifi_p0..wifi_p4 (secret), most recently joined first. wifi_ssid/wifi_pass
// above stay the pair in use (the front of the list). All of these are called with wifi_service.c's saved_lock held.
struct wifi_saved_s;
// Loads the list (empty when nothing is saved; v2 migrates no older pair into it, #216); false when no store is bound
bool wifi_saved_load(struct wifi_saved_s *l);
// Removes one network. was_active: it was the pair in use; the next entry (if any) takes its place, left = entries remain
bool wifi_cred_forget(char const *ssid, bool *was_active, bool *left);
// Makes a saved network the pair in use (front of the list); false if it is not saved. No change if it is already.
bool wifi_cred_activate(char const *ssid);
