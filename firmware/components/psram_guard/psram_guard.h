#pragma once

// PSRAM watchdog (#40, debug feature): canary blocks in PSRAM, filled with an address-dependent pattern and verified
// every ~100 ms by a low-priority task on core 0. Damage is logged with ESP_LOGW (so it reaches GET /log), the block is
// refilled and watching goes on. "fw psram" shows the totals. Bytes copied into PSRAM read back wrong during Wi-Fi
// firmware updates (#37) and the screen flickers (#31); this is there to catch the writer or the timing that does it.
//
// Cost: 4 x 64 KB + two small blocks of PSRAM, plus one more 64 KB block during update mode. Each pass reads ~270 KB
// of PSRAM (about 2.7 MB/s at a 100 ms period); not measured on hardware, "fw psram" shows the last and longest pass.

// First line of app_main: registers the PSRAM with the heap minus the learned bad pages (#86). Until this runs there
// is no PSRAM heap (the IDF's registration is held back), so nothing may allocate in PSRAM before it.
void psram_guard_heap_start(void);

// After the heaps are up (app_main, early): boot scan, boot check, canaries, the task, "fw psram"
void psram_guard_start(void);

// One more canary while update mode runs (#37 hits then); idempotent. From any task.
void psram_guard_update_begin(void);
void psram_guard_update_end(void);

// Before freeing p: true when p is NULL, internal RAM or inside the PSRAM heap (heap_caps_free would take it). Otherwise
// heap_caps_free would abort "outside heap areas" without naming the pointer (#89): this logs what p looks like (a
// quarantine hole, PSRAM outside the pools, not RAM; whether one flipped bit would make it a heap address), counts it
// for "fw psram" and returns false, so the caller leaks the block instead of taking the badge down.
bool psram_guard_free_ok(void const *p);
