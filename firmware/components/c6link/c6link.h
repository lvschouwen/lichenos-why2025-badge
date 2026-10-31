#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_join.h"
#include "wifi_list.h"

// The C6 coprocessor (#61): make sure it runs our image, bring up ESP-Hosted, join Wi-Fi.

// True once ESP-Hosted talks to the C6, with or without Wi-Fi (no credentials, wrong password, out of range)
bool c6link_start(void);
bool c6link_up(void);
// A firmware update is being received (#76): wifi_scan refuses from now on (until the restart)
void c6link_set_updating(void);
// Copies the station's IPv4 address into ip; false while there is none
bool wifi_has_ip(char *ip, size_t cap);
// The cached signal strength of the connected AP (read every 5 s by the c6wifi task); false while not connected or
// before the first reading
bool wifi_rssi(int8_t *out);
// A join is under way: a trial (PENDING/TRYING) or the first connect after boot, until it gets an IP or fails
bool wifi_joining(void);
// True when a network is saved (NVS badge_fw), whether or not it is connected right now
bool wifi_has_credentials(void);
// Blocking Wi-Fi scan, at most about 6 s: never call it on the LVGL task. Fills out with up to max cleaned entries
// (wifi_list_clean: no empty names, one per SSID, strongest first). False if the scan could not run.
bool wifi_scan(wifi_ap_t *out, size_t max, size_t *n);
// Starts a trial join of the pair (empty password for an open network) and returns at once: the pair is applied to the
// driver but saved to NVS (badge_fw wifi_ssid / wifi_pass) only once it gets an IP. Without one in 20 s, or on an
// authentication failure, the previous pair is put back and NVS was never touched. False if the pair is invalid or the
// request could not be queued. Follow the outcome with wifi_join_state(); it does not depend on any screen.
bool wifi_set_credentials(char const *ssid, char const *pass);
// TRYING / JOINED / FAILED_KEPT_OLD (NONE before the first trial). tried gets the SSID of the last trial; kept the SSID
// that is back after a failure ("" if no network was saved).
wifi_join_state_t wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap);
