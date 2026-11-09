#pragma once

// When the DMA-capable heap's low-water mark fell, and what was running then (#256). The heap only keeps the lowest
// free figure ever seen, not when it was reached: a sampler feeds the mark in now and then, and every drop of at
// least DMA_LOW_STEP since the last record is kept with its uptime and the task running on the other core, so a soak
// can name the moment of the low point. Pure (no ESP-IDF), host-tested in host_tests/dma_low_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMA_LOW_STEP     2048 // a smaller drop since the last record is the heap's everyday churn
#define DMA_LOW_KEEP     4    // the last drops; the newest is the low point
#define DMA_LOW_TASK_CAP 16   // configMAX_TASK_NAME_LEN on the badge

typedef struct {
    uint64_t at_ms;    // uptime of the sample that saw the drop (the drop happened since the sample before)
    uint32_t min;      // the heap's minimum free then
    uint32_t fell;     // below the record before it (0 for the first sample)
    uint32_t free_now; // free at that sample: near min = still held, back up = a short peak
    char     task[DMA_LOW_TASK_CAP];
} dma_low_mark_t;

typedef struct {
    dma_low_mark_t first;               // the first sample: everything boot took before the sampler ran
    dma_low_mark_t mark[DMA_LOW_KEEP];  // ring of the last drops after it
    uint32_t       drops;               // drops recorded after the first sample
    bool           started;
} dma_low_t;

// A sample: min and free_now are the heap's figures now, task what ran then (NULL = "-"). True when it was recorded.
bool dma_low_feed(dma_low_t *l, uint32_t min, uint32_t free_now, uint64_t at_ms, char const *task);

// Lines for "fw mem", one per call: i 0 is the first sample, 1.. the kept drops oldest first. 0 when i is past the end.
// "mem dma_low first min 90000 fell 0 at 12.345s free 99000 task -"
// "mem dma_low drop 3/7 min 68423 fell 21577 at 3600.250s free 70000 task ota" (fell: since the record before it)
size_t dma_low_line(dma_low_t const *l, unsigned i, char *out, size_t cap);
