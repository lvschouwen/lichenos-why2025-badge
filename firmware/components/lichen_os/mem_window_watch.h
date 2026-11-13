#pragma once

#include "sdkconfig.h"

// "fw mem window [reset]" (#410, mem_window.h): a low-water mark per heap that can be restarted, beside the
// since-boot one. Developer mode only; with LICHEN_DEV_COMMANDS off (the release build) these are empty and nothing
// is sampled.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
// One sample of the three heaps. On dma_watch.c's esp_timer tick (every 250 ms): no task or timer of its own, no
// allocation, no log.
void mem_window_watch_sample(void);
// Hooks "mem window" into fw mem (once, from lichen_mem_console_register).
void mem_window_watch_register(void);
#else
static inline void mem_window_watch_sample(void) {
}
static inline void mem_window_watch_register(void) {
}
#endif
