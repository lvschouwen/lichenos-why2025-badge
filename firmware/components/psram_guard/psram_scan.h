#pragma once

#include <stddef.h>

// Boot-time PSRAM map and quarantine (#86). Before anything else takes the memory, psram_scan_run() grabs the free
// PSRAM in 64 KB chunks, scans the suspect window with a sparse and three dense passes, keeps every chunk that touches
// a bad range plus margin allocated for good, and frees the rest. Pure range math: psram_quar.c.

// First thing in psram_guard_start(). Logs the map and the verdict at WARN.
void psram_scan_run(void);

// "fw psram": quarantine lines (ranges, held bytes, bits, direction, map)
void psram_scan_print(void);

// "fw psram qwatch <ms>": fill the held quarantine chunks, wait, read back: a writer or slow decay shows up here, with
// everything else (display, Wi-Fi, LVGL) running. Replies with the bad bytes per chunk.
void psram_scan_watch(unsigned ms);

// A buffer somebody else owns (display framebuffer ...), shown in "fw psram" next to the quarantine
void psram_scan_note_region(char const *name, void const *addr, size_t len);
