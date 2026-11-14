#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifi_join.h"
#include "wifi_list.h"
#include "wifi_regdom.h"

// Lichen's Wi-Fi service (#61, #66, #108): the c6wifi task owns the esp_wifi driver that ESP-Hosted carries over the C6
// link, joins and keeps the connection, sets the clock over SNTP and answers `fw wifi` / `fw net`. Moved out of c6link
// (#223): c6link (L2) reports the link through the net hooks this registers, this (L3) asks c6link_up/updating.

// The OS radio switch, from main.c (the switch lives behind the gate, lichen_radio.h, which an L3 service may not call).
// Any member may be NULL: without get the console shows wifi_enabled(), without set `fw wifi on|off` says the switch is
// not there, without airplane it is taken as off.
typedef struct {
    bool (*get)(void);     // the saved Wi-Fi switch
    bool (*set)(bool on);  // save it; the switch's listener then calls wifi_enable
    bool (*airplane)(void);
} wifi_radio_hooks_t;

// Once at boot, before wifi_service_init: where the saved-list copies come from (PSRAM, #249). Unbound, the saved list
// reports busy, Forget fails and failover between saved networks stops (logged once).
void wifi_bind_alloc(void *(*alloc)(size_t size), void (*release)(void *p));
// Once at boot, before c6link_start: the console commands (clock, net, wifi), the c6wifi task and the hooks c6link calls
// when the link is up, missing or being re-attached. radio may be NULL (all defaults).
void wifi_service_init(wifi_radio_hooks_t const *radio);

// The OS Wi-Fi switch (#108; main.c wires lichen_radio to it): off stops the Wi-Fi driver on the C6 and closes everything
// that rides on it (IP, update server, scans and joins are refused), on starts it and reconnects with the saved pair.
// The C6 link itself and LoRa are not touched, and the C6 watch (c6_watch) never sees it as an outage. Returns at once;
// the c6wifi task does the work. May be called before wifi_service_init.
void wifi_enable(bool on);
bool wifi_enabled(void);
// Copies the station's IPv4 address into ip; false while there is none
bool wifi_has_ip(char *ip, size_t cap);
// Messages to the c6wifi task dropped for a full queue, since boot (fw soak, #419). Any task, no lock.
uint32_t wifi_queue_drops(void);
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
// The same scan without wifi_list_clean: every AP the driver reports (up to 32), with its channel, duplicates and hidden
// names included. For the Wi-Fi analyser (#159), which counts APs per channel.
bool wifi_scan_raw(wifi_ap_t *out, size_t max, size_t *n);
// Forgets one saved network (#160). If it was the pair in use, the next saved one takes over (or the radio is left
// without a network). False if it was not saved. The password never leaves NVS. Not on the c6wifi task.
bool wifi_forget_network(char const *ssid);
// The saved SSIDs (most recently joined first), read under the list lock; no passwords leave lichen_net (#240).
// Returns how many were written to out (at most max), or SIZE_MAX when the list is busy (the c6wifi task holds it;
// waits at most 50 ms, #249).
size_t wifi_saved_names(char (*out)[33], size_t max);
// Starts a trial join of the pair (empty password for an open network) and returns at once: the pair is applied to the
// driver but saved to NVS (badge_fw wifi_ssid / wifi_pass) only once it gets an IP. Without one in 20 s, or on an
// authentication failure, the previous pair is put back and NVS was never touched. False if the pair is invalid or the
// request could not be queued. Follow the outcome with wifi_join_state(); it does not depend on any screen.
bool wifi_set_credentials(char const *ssid, char const *pass);
// TRYING / JOINED / FAILED_KEPT_OLD (NONE before the first trial). tried gets the SSID of the last trial; kept the SSID
// that is back after a failure ("" if no network was saved).
wifi_join_state_t wifi_join_state(char *tried, size_t tried_cap, char *kept, size_t kept_cap);

// The driver's applied country, protocol, bandwidth, TX power and power save for the settings registry (#311), with
// what the P4 asked for. live: read them now over the RPCs (waits for the C6, so never on the LVGL task) when the link
// and the driver are up; otherwise (and when that read got nothing) *out is the cache, read after each Wi-Fi start and
// association. Returns true when *out was read just now.
bool wifi_readback(wifi_readback_t *out, bool live);
// Power save none (false) or min (true), not saved (#146): applied now when Wi-Fi runs, else at the next start. False
// when the driver refused it.
bool wifi_set_ps(bool min);
