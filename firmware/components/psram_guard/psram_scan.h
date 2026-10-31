#pragma once

#include <stddef.h>
#include <stdint.h>

// Boot-time PSRAM quarantine and map (#86). The IDF's registration of the PSRAM with the heap is held back (linker
// wrap of heap_caps_add_region_with_caps); psram_scan_release_heap() registers it cut around the learned bad pages
// (+- one page), so those pages are holes nobody can allocate: the quarantine is guaranteed by construction. Then
// psram_scan_run() grabs the free PSRAM in 64 KB chunks, scans the suspect window with a sparse and three dense passes
// to learn new pages (holes from the next boot on; the chunks holding them stay allocated until then) and frees the
// rest. Pure range math: psram_quar.c; learned set: psram_learn.c / psram_learned.c.

// First line of app_main (before anything allocates in PSRAM): loads the learned set (NVS) and registers the heap.
void psram_scan_release_heap(void);

// From psram_guard_start(). Logs the map and the verdict at WARN.
void psram_scan_run(void);

// "fw psram": quarantine lines (ranges, held bytes, bits, direction, map)
void psram_scan_print(void);

// "fw psram qwatch <ms>": fill the quarantine (holes + held chunks), wait, read back: a writer or slow decay shows up here, with
// everything else (display, Wi-Fi, LVGL) running. Replies with the bad bytes per chunk.
void psram_scan_watch(unsigned ms);

// "fw psram stress <rounds>": alternating dense passes (pattern, inverted) over the held quarantine, bad bytes per round
void psram_scan_stress(unsigned rounds);

// A buffer somebody else owns (display framebuffer ...), shown in "fw psram" next to the quarantine
void psram_scan_note_region(char const *name, void const *addr, size_t len);
// Pages for the delay-line sweep (psram_sweep.c): quarantined 4 KB pages that failed before, and pages from the heap
// outside the quarantine as controls. Returns the number of bad pages; *n_good the number of good ones.
size_t psram_scan_test_pages(uint8_t **bad, size_t max_bad, uint8_t **good, size_t max_good, size_t *n_good);
