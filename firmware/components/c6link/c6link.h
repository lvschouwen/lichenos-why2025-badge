#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c6_watch.h"
#include "esp_event.h"
#include "wifi_join.h"
#include "wifi_list.h"

// The C6 coprocessor (#61): make sure it runs our image, bring up ESP-Hosted, join Wi-Fi.

// True once ESP-Hosted talks to the C6, with or without Wi-Fi (no credentials, wrong password, out of range)
bool c6link_start(void);
// False while the C6 supervisor (c6sup, #14) recovers the link, true again once it is back
bool c6link_up(void);

// Posted on the default event loop when the link goes down and when it is back (#14), for observers. The users of the
// link are stopped and restarted through c6link_set_hooks(), in a fixed order, before these are posted.
ESP_EVENT_DECLARE_BASE(C6LINK_EVENT);
enum { C6LINK_EVENT_DOWN, C6LINK_EVENT_UP };

// What the supervisor needs from the rest of the badge (#14 T-B), from main.c: c6link depends on neither the board
// nor LoRa. All run on the c6sup task; any member may be NULL.
typedef struct {
    void (*down)(void); // the link is going away: stop its users (LoRa)
    void (*up)(void);   // it is back: keyboard_reinit, then backlight_reapply, then LoRa, in that order
    // Ask the C6 something cheap and wait at most timeout_ms; true with its uptime when it answered
    bool (*probe)(uint32_t timeout_ms, uint32_t *c6_uptime_s);
    // The C6 is not coming back: ask whether to restart (Lucas: a warning dialog with a countdown). Answer later with
    // c6link_fallback_answer(); false when it could not ask (the badge then restarts at once)
    bool (*ask_restart)(void);
    void (*ask_cancel)(void); // the C6 came back while the question was open: take it away
} c6link_hooks_t;
void c6link_set_hooks(c6link_hooks_t const *hooks);
// The answer to ask_restart: true restarts the badge (Enter, or the countdown ran out), false stays without the C6
// (Esc; the slow retry goes on). Safe from any task.
void c6link_fallback_answer(bool restart);
// The C6 is being recovered or is gone: the state in words ("recovering 2/3", "down (no answer)") for the status bar
// and About; false while it is up. Quick, safe from the LVGL task.
bool c6link_trouble(char *out, size_t cap);
// Every peer-data send to the C6 (lora_set_send_hook): failures in a row are the soft signal that it may be gone (#14)
void c6link_note_lora_send(bool ok);

// A C6 reset at runtime (#14): what the watch knows, for `fw c6`, the status bar and About. Safe from any task.
typedef struct {
    c6w_state_t  state;
    c6w_source_t source;           // what started the current (or the last) outage
    uint32_t     restarts;         // outages the C6 came back from
    uint32_t     last_recovery_ms; // how long the last one took
    uint8_t      attempt;          // re-attach attempts in the current outage
    char         reason[C6W_REASON_MAX]; // the last recovery in words ("" before the first)
} c6_status_t;
void c6link_status(c6_status_t *out);
// The keyboard found the TCA8418 reset, which only C6 EN does: hand it to keyboard_set_reset_hook() (#14)
void c6link_note_keyboard_reset(void);
// A file under /storage/c6 was written or deleted: forget the verified bundle so the next boot checks the C6 fully (#36).
// Invariant: EVERY writer of /storage/c6 must call this before touching the files. The boot-time skip key is built from
// file sizes and the .md5 text only, so a .bin changed behind its back would still look verified.
void c6link_bundle_changed(void);
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
