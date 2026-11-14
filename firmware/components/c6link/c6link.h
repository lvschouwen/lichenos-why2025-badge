#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "c6_hooks.h"
#include "c6_watch.h"
#include "esp_event.h"

// The C6 coprocessor (#61): make sure it runs our image and bring up ESP-Hosted. Wi-Fi rides on it but lives in
// lichen_net (wifi_service.h, #223): c6link is an L2 driver and reaches the layers above only through the hooks of
// c6_hooks.h (boot step, Wi-Fi driver), given before c6link_start.

// True once ESP-Hosted talks to the C6, with or without Wi-Fi (no credentials, wrong password, out of range). Ends with
// the net hook's attached (Wi-Fi up) or, without a link, no_link.
bool c6link_start(void);
// Before c6link_start (#36): how to ask the running C6 app for its ELF sha256 (IDENT; main.c gives lora_ident, c6link
// does not depend on LoRa). Without it no boot ever proves the C6, so every boot checks it fully.
#define C6LINK_IDENT_LEN 32
// The runtime re-attach asks IDENT only when this says so (main.c gives lora_up): without a LoRa task the question is
// not asked and the verify flag stays as it is
void c6link_set_ident_ready(bool (*ready)(void));
void c6link_set_ident(bool (*ident)(uint8_t out[C6LINK_IDENT_LEN], uint32_t timeout_ms));
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
// The heartbeat's counters since boot (#277): beats received, misses, beats lost between the chips. For fw soak (#409):
// a copy under the watch's spinlock, no reason text. Any task.
void c6link_hb_counts(uint32_t *beats, uint32_t *misses, uint32_t *dropped);
// Supervisor messages dropped for a full queue, since boot (fw soak, #419). Any task, no lock.
uint32_t c6link_queue_drops(void);
// The keyboard found the TCA8418 reset, which only C6 EN does: hand it to keyboard_set_reset_hook() (#14)
void c6link_note_keyboard_reset(void);
// A file under /storage/c6 was written or deleted: forget the verified bundle so the next boot checks the C6 fully (#36).
// Invariant: EVERY writer of /storage/c6 must call this before touching the files. The boot-time skip key is built from
// file sizes and the .md5 text only, so a .bin changed behind its back would still look verified.
void c6link_bundle_changed(void);
// c6link_bundle_changed ran this boot: the next boot flashes what changed (#420). Any task.
bool c6link_bundle_pending(void);
// A firmware update is being received (#76): wifi_scan refuses from now on (until the restart)
void c6link_set_updating(void);
// True once c6link_set_updating or c6link_hold_in_reset ran: no scans, no RSSI polls, no C6 recovery
bool c6link_updating(void);
// The C6 flasher is writing or checking the C6 (the boot's image check, #353: no speed restart meanwhile). Any task
bool c6link_flashing(void);
// One Wi-Fi frame pool of the link (#260), for the stress run's samples (#278, reached through lichen_sys_mem.h):
// blocks in use now and the most since boot, TX waits and drops. False (zeros) when the pools are off. Any task.
bool c6link_frame_pool(bool tx, uint16_t *in_use, uint16_t *peak, uint32_t *waits, uint32_t *drops);
// Power off (#65): holds the C6 (and the TCA8418) in reset through the P4's deep sleep. Nothing on the link works after.
void c6link_hold_in_reset(void);
