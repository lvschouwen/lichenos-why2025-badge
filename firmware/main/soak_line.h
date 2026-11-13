#pragma once

// "fw soak" (#409): the badge's state in key=value lines for an unattended sampler (the soak, #407). Numbers and short
// lowercase enum words only: no names, addresses, ids or text, so a line may be pasted into a public issue.
//
// The contract for tools (pinned by host_tests/soak_line_test.c):
// - SOAK_LINES lines, "soak1 ..." to "soak3 ...". Each starts with its name, then v (SOAK_FORMAT), boot and up, so a
//   line read on its own says which boot and moment it belongs to; the keys of one "fw soak" come from one snapshot.
// - Keys are separated by single spaces and never change order. A new key is appended to the last line; when that
//   line would not fit, a new line soak4 starts. Removing or reordering a key bumps SOAK_FORMAT.
// - A number that cannot be read now is SOAK_NA (-1), an enum that cannot be read is "na"; wdbm (Wi-Fi signal) is 0
//   when there is no reading (a real one is below 0). A number above its key's ceiling prints the ceiling.
// - Every line, at every key's ceiling, fits SOAK_LINE_MAX: the console's reply limit (console.c REPLY_MAX).
// Pure (no ESP-IDF headers): the glue that reads the values is soak_cmd.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOAK_FORMAT   1
#define SOAK_LINES    3
#define SOAK_LINE_MAX 384 // a 383-character line and its NUL: console.c's REPLY_MAX, past which the Wi-Fi reply is cut
#define SOAK_NA       (-1)
#define SOAK_NA_WORD  "na"

// One snapshot. Every field is a number (or an enum's number, see the word tables in soak_line.c); soak_init sets all
// of them to SOAK_NA, so a value the glue does not fill prints as unknown, never as a plausible 0.
typedef struct {
    int64_t boot;       // boots of this badge (crash.c's NVS counter): +1 on every boot, whatever the reset
    int64_t up_ms;      // P4 uptime
    // soak1: stability and memory
    int64_t reset;      // esp_reset_reason_t of this boot (on this board a power-on and the CH340 reset read as wdt)
    int64_t reset_crash; // 1: crash.c judged this boot to follow a crash (a panic, a watchdog with a fresh core dump)
    int64_t crashes;    // crash records since the last fw crash clear
    int64_t slot;       // running app slot: 0 (ota_0) or 1 (ota_1)
    int64_t slot_ok;    // 1: the running image is valid, 0: on trial (a reset rolls it back)
    int64_t int_free, int_min, int_big, int_win, int_wx; // internal RAM: free, since-boot min, largest block,
    int64_t dma_free, dma_min, dma_big, dma_win, dma_wx; // the fw mem window minimum and its exact flag (#410)
    int64_t ps_free, ps_min, ps_big, ps_win, ps_wx;      // (DMA-capable internal RAM, PSRAM: the same)
    int64_t lp_free, lp_min; // LP RAM (the tasks placed there, #268)
    int64_t dma_drops;  // DMA low-water records after the first (#256): rises when the DMA minimum falls by a step
    // soak2: watchdogs and the C6 link
    int64_t lock_hits;  // LVGL calls without the LVGL lock (#225)
    int64_t canary;     // PSRAM canary hits since boot (#40, #121)
    int64_t c6_state;   // c6w_state_t
    int64_t c6_restarts; // C6 outages it came back from
    int64_t hb_beats, hb_misses, hb_dropped; // C6 heartbeats since boot (#277)
    int64_t pool_tx_drops, pool_rx_drops;    // Wi-Fi frame pool drops (#260)
    int64_t lora_state; // lora_state_t of the C6's last status
    int64_t lora_packets, lora_crc; // the C6's counters: they restart with the C6, so they belong to c6up
    int64_t c6_up_s;    // the C6's uptime in that status
    int64_t lora_age_s; // how old that status is
    int64_t lora_rx, lora_tx; // packets the P4 got from the C6 and sent on air, since the P4 boot
    // soak3: MeshCore, Wi-Fi, temperature, storage, power, keyboard, backlight
    int64_t mesh_rx, mesh_tx, mesh_failed, mesh_timeouts, mesh_refused, mesh_rx_dropped;
    int64_t air_logged, air_dropped; // the air log: notes logged since boot, notes dropped (no ring)
    int64_t wifi_dbm;   // 0: no reading
    int64_t wifi_up;    // 1: has an IP
    int64_t thermal;    // th_level_t
    int64_t storage_free; // bytes on /storage (the last measurement, at most 10 min old)
    int64_t sd_state;   // sd_state_t
    int64_t sd_ok;      // 1: a card is mounted
    int64_t sd_free;    // bytes
    int64_t sd_errors;  // failed SD batches since the last mount
    int64_t battery;    // bat_gauge_t
    int64_t battery_mv; // the cell voltage, when a reading passed the checks
    int64_t kbd_ok;     // 1: the TCA8418's last readback had its setup, 0: it did not answer
    int64_t kbd_resets; // TCA8418 resets the readback found (C6 EN, #14)
    int64_t bl_req, bl_app;   // display backlight: last level sent to the C6, the level the C6 reports applied
    int64_t kbl_req, kbl_app; // keyboard backlight: the same
} soak_t;

// Every field SOAK_NA
void soak_init(soak_t *s);

// Line n (1..SOAK_LINES) into out, NUL-terminated; returns its length. 0 (out "" when cap > 0) for another n or when
// cap is too small for the whole line: a line is never cut.
size_t soak_line(soak_t const *s, unsigned n, char *out, size_t cap);
