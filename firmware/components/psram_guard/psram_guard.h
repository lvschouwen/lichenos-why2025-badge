#pragma once

#include <stdbool.h>

// PSRAM watchdog (#40, #93): canary blocks in PSRAM, filled with an address-dependent pattern and verified every
// ~100 ms by a low-priority task on core 0. Damage is logged with ESP_LOGW (so it reaches GET /log), the block is
// refilled and watching goes on. "fw psram" shows the tuning the IDF chose, the boot check and the canary totals.
// History: at 200 MHz this board flipped bit 8/9 of PSRAM words (#86, #89); since 80 MHz (#93) the guard only watches,
// the quarantine machinery (boot scan, learned pages, heap holes, sweep, stress, ldo) is gone: last commit with it is
// 7392597.
//
// Cost: 4 x 64 KB + two small blocks of PSRAM, plus one more 64 KB block during update mode. Each pass reads ~270 KB
// of PSRAM; about 7 ms and 6-7 % of core 0 at 80 MHz ("fw psram" shows the last and longest pass).

// After the heaps are up (app_main, early): boot check, canaries, the task, "fw psram"
void psram_guard_start(void);

// One more canary while update mode runs (#37 hits then); idempotent. From any task.
void psram_guard_update_begin(void);
void psram_guard_update_end(void);

// Before freeing p: true when p is NULL, internal RAM or inside the PSRAM heap (heap_caps_free would take it). Otherwise
// heap_caps_free would abort "outside heap areas" without naming the pointer (#89): this logs what p looks like (PSRAM
// outside the pools, not RAM; whether one flipped bit would make it a heap address), counts it for "fw psram" and
// returns false, so the caller leaks the block instead of taking the badge down.
bool psram_guard_free_ok(void const *p);
