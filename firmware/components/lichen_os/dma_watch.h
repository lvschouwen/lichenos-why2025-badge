#pragma once

#include <stdbool.h>
#include <stdint.h>

// The DMA low-water watch (#256, dma_low.h): lichen_mem.c starts it with the fw mem lines and prints its lines there.
void dma_watch_start(void); // once; a second call does nothing
void dma_watch_report(void); // "mem dma_low ..." lines, on the console task
// DMA low-water records after the first sample (#256): fw soak (#409). A copy under the record's spinlock, any task;
// false while the watch has not sampled yet.
bool dma_watch_drops(uint32_t *drops);
// "fw mem diff" (#547, developer images): the task that made the block at addr, from a ring of the last 24 internal
// allocations of 4 KB or more; size is the block's size and an entry must fit in it. False when the ring no longer holds it
bool dma_watch_owner(uint32_t addr, uint32_t size, char *task, size_t cap);
