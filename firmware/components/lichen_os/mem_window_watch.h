#pragma once

#include "mem_window.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

// "fw mem window [reset]" (#410, mem_window.h): a low-water mark per heap that can be restarted, beside the
// since-boot one. Developer mode only; with LICHEN_DEV_COMMANDS off (the release build) these are empty and nothing
// is sampled.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
// One sample of the three heaps. On dma_watch.c's esp_timer tick (every 250 ms): no task or timer of its own, no
// allocation, no log.
void mem_window_watch_sample(void);
// Hooks "mem window" into fw mem (once, from lichen_mem_console_register).
void mem_window_watch_register(void);
// The window as the last sample left it, per heap (mem_window_heap_t order): its minimum and whether that is exact
// (mem_window.h). False before the first sample. For fw soak (#409): a copy under the window's spinlock, any task.
bool mem_window_watch_read(uint32_t min[MEM_WINDOW_HEAPS], bool exact[MEM_WINDOW_HEAPS]);
// The heaps' figures now, with the window's caps (fw mem's lines): free, since-boot minimum, largest block (a walk of
// the heap under its lock, as fw mem's). lp: LP RAM (MALLOC_CAP_RTCRAM, #268), false when the badge has none. For fw
// soak (#409); any task but not an interrupt.
typedef struct {
    uint32_t free, min, big;
} mem_heap_fig_t;
bool mem_window_watch_heaps(mem_heap_fig_t out[MEM_WINDOW_HEAPS], mem_heap_fig_t *lp);
#else
static inline void mem_window_watch_sample(void) {
}
static inline void mem_window_watch_register(void) {
}
typedef struct {
    uint32_t free, min, big;
} mem_heap_fig_t;
static inline bool mem_window_watch_heaps(mem_heap_fig_t out[MEM_WINDOW_HEAPS], mem_heap_fig_t *lp) {
    (void)out;
    (void)lp;
    return false;
}
static inline bool mem_window_watch_read(uint32_t min[MEM_WINDOW_HEAPS], bool exact[MEM_WINDOW_HEAPS]) {
    (void)min;
    (void)exact;
    return false;
}
#endif
