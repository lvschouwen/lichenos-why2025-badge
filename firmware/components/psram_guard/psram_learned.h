#pragma once

#include "psram_learn.h"

// The learned bad extent in NVS (#86). Every detector reports what it found here; the range is stored only when it
// grows. Pure part: psram_learn.c.

// Early in psram_guard_start: brings NVS up (nvs_flash_init is idempotent) and loads the record; empty when missing,
// from another PSRAM speed or version.
void psram_learned_load(void);

// Bad bytes [lo, hi] seen by who. Any task. Logs and stores when the range grows.
void psram_learned_feed(uintptr_t lo, uintptr_t hi, char const *who);

// A bad byte at addr: its page goes into the failure map (kept in RAM; the next psram_learned_feed stores it)
void psram_learned_mark(uintptr_t addr);

// Marks every 4 KB page between byte offsets first..last of mem[0..len) that holds a byte differing from the pattern (check as in psram_pattern_check_x
// with tag = the logical address of mem[0] and mask), re-reading what the check just read.
void psram_learned_mark_buf(uint8_t const *mem, size_t len, uintptr_t tag, uint32_t mask, size_t first, size_t last);

// Copy of the record (empty: psram_learn_empty)
psram_learn_t psram_learned_get(void);

// "fw psram forget": drop the NVS record and the in-memory range (the quarantine of this boot stays)
void psram_learned_forget(void);

// "fw psram" lines
void psram_learned_print(void);
