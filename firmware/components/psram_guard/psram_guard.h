#pragma once

// PSRAM watchdog (#40, debug feature): canary blocks in PSRAM, filled with an address-dependent pattern and verified
// every ~100 ms by a low-priority task on core 0. Damage is logged with ESP_LOGW (so it reaches GET /log), the block is
// refilled and watching goes on. "fw psram" shows the totals. Bytes copied into PSRAM read back wrong during Wi-Fi
// firmware updates (#37) and the screen flickers (#31); this is there to catch the writer or the timing that does it.
//
// Cost: 4 x 64 KB + two small blocks of PSRAM, plus one more 64 KB block during update mode. Each pass reads ~270 KB
// of PSRAM (about 2.7 MB/s at a 100 ms period); not measured on hardware, "fw psram" shows the last and longest pass.

// After the heaps are up (app_main, early): allocates the canaries, starts the task, registers "fw psram"
void psram_guard_start(void);

// One more canary while update mode runs (#37 hits then); idempotent. From any task.
void psram_guard_update_begin(void);
void psram_guard_update_end(void);
