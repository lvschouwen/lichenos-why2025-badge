#pragma once

// "fw soak" (#409): the badge's state in key=value lines for an unattended sampler (the soak, #407). Numbers and short
// lowercase enum words only: no names, addresses, ids or text, so a line may be pasted into a public issue.
//
// The contract for tools (pinned by host_tests/soak_line_test.c):
// - SOAK_LINES lines, "soak1 ..." to "soak4 ...". Each starts with its name, then v (SOAK_FORMAT), boot and up, so a
//   line read on its own says which boot and moment it belongs to; the keys of one "fw soak" come from one snapshot.
// - Keys are separated by single spaces and never change order. A new key is appended to the last line; when that
//   line would not fit, a new line starts. Removing or reordering a key bumps SOAK_FORMAT; adding a key or a line does
//   not (soak4, #419, kept v=1: a sampler that reads the keys it knows by name loses nothing). soak3 has 11
//   characters left at its ceilings: room for the Wi-Fi disconnect count (its own issue) and no more. soak4 holds
//   the slow-path counters; the mesh ones (mslow, mslowms, mslowfile, jpend, jdrop) are -1 while MeshCore is stopped.
// - A number that cannot be read now is SOAK_NA (-1), an enum that cannot be read is "na"; wdbm (Wi-Fi signal) is 0
//   when there is no reading (a real one is below 0). A number above its key's ceiling prints the ceiling.
// - Every line, at every key's ceiling, fits SOAK_LINE_MAX: the console's reply limit (console.c REPLY_MAX).
// - Reboots: a boot higher than the last sample's is a reboot. boot=-1 means the counter is unknown this boot, not a
//   change. A boot LOWER than the last sample's (or an up lower than the last one) means the counter's storage was
//   restarted, which counts as a reboot too: NVS is erased and the count starts again at 1 after the boot-time
//   erase-and-retry of an NVS with a broken layout (main.c nvs_boot), a full flash erase, and a rollback from an
//   image with encrypted settings to one without the guard; the #116 migration keeps it, except for a power cut
//   inside its erase window.
// - Crashes: rst=wdt rstcrash=0 is what a power cycle, an EN reset through the USB adapter and a watchdog reset
//   without a core dump all print on this board. Count crashes from rstcrash and boot (with crash), never from rst
//   alone.
//   rstcrash=1 is sticky across the PSRAM re-arm: after a crash at 120 MHz the badge restarts by software about 20 s
//   later ("psram 120 re-arm after crash"), so a sampler would only ever see that second boot. That boot (rst=sw)
//   reports rstcrash=1 too, meaning "the previous boot crashed". It follows from the recorded restart history, not the
//   crash log, so fw crash clear does not hide it; the boot after it reads 0 again. One crash can therefore show as
//   two boots (the panic boot and the re-arm boot, under 20 s apart), so up to two samples can show one crash: when
//   two consecutive samples have rstcrash=1 and boot advanced by exactly 1, count one crash. Compare crash (which
//   fw crash clear resets) only as a cross-check. The meaning widened and no key moved, so SOAK_FORMAT stays 1.
// Pure (no ESP-IDF headers): the glue that reads the values is soak_cmd.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOAK_FORMAT   1
#define SOAK_LINES    4
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
    // soak4: slow paths (#419), counted where each warning line is printed, since boot
    int64_t mesh_slow, mesh_slow_max, mesh_slow_file; // MeshCore loops over its slow limit: count, longest ms, longest file ms
    int64_t store_last_ms, store_max_ms; // the OS's /storage count: the last and the longest wall time (ms)
    int64_t notice_slow, notice_slow_max; // OS notice steps over their slow limit: count, longest ms
    int64_t ui_refused, ui_wait_max;     // ui calls refused for want of the LVGL lock: count, longest wait asked (ms)
    int64_t wifi_qfull, c6_qfull;        // messages dropped for a full queue: the Wi-Fi service, the C6 supervisor
    int64_t toast_drops, notice_drops;   // toasts dropped from the full waiting list; notices that found no room
    int64_t journal_pend_hw, journal_dropped; // MeshCore history: highest pending bytes, records not journaled
    int64_t c6_send_busy;                // times the C6 link went down with a send still inside ESP-Hosted
} soak_t;

// 1 when this boot follows a crash: crash.c judged so itself (crash_now), or it is the software restart of the PSRAM
// re-arm after a crash (reset_is_sw, and newest_restart, the newest line of the restart history, names that restart).
// NULL newest_restart: no history. Pure, so the sticky rule is pinned by a host test.
int soak_reset_crash(bool crash_now, bool reset_is_sw, char const *newest_restart);

// Every field SOAK_NA
void soak_init(soak_t *s);

// Line n (1..SOAK_LINES) into out, NUL-terminated; returns its length. 0 (out "" when cap > 0) for another n or when
// cap is too small for the whole line: a line is never cut.
size_t soak_line(soak_t const *s, unsigned n, char *out, size_t cap);
