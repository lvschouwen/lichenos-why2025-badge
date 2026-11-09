#pragma once

// When the DMA-capable heap's low-water mark fell, and who took the most then (#256). The heap only keeps the lowest
// free figure ever seen, not when it was reached: a sampler feeds the mark in now and then, and once the mark is
// DMA_LOW_STEP below where the newest record started, a new record starts with its uptime, the free figure then and
// the largest internal allocation in that sample's window (its size and its task). A smaller fall moves the newest
// record's min along (and its big/task when larger), so the last line always shows the heap's true minimum. Pure (no ESP-IDF), host-tested in
// host_tests/dma_low_test.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DMA_LOW_STEP     2048 // a smaller fall is the heap's everyday churn: it updates the newest record in place
#define DMA_LOW_KEEP     4    // the last drops; the newest is the low point
#define DMA_LOW_TASK_CAP 16   // configMAX_TASK_NAME_LEN on the badge

typedef struct {
    uint64_t at_ms;    // uptime of the sample that saw the drop (the drop happened since the sample before)
    uint32_t min;      // the heap's minimum free; the newest record follows it down
    uint32_t fell;     // below the record before it, as it stood (for the first: below its own first sample)
    uint32_t free_now; // free at that sample: near min = still held, back up = a short peak
    uint32_t big;      // bytes of the largest internal allocation in the windows where this record's min fell (0: none)
    char     task[DMA_LOW_TASK_CAP]; // the task that made it ("-": none; an ISR's is the task it interrupted)
} dma_low_mark_t;

typedef struct {
    dma_low_mark_t first;              // the first sample: everything boot took before the sampler ran
    dma_low_mark_t mark[DMA_LOW_KEEP]; // ring of the last drops after it
    uint32_t       drops;              // drops recorded after the first sample
    uint32_t       ref;                // min when the newest record started: the next step is measured from here
    bool           started;
} dma_low_t;

// One sample: min and free_now are the heap's figures now, big and task the largest internal allocation since the
// sample before (0 and NULL or "" when none). True when it started a record (the caller logs it).
bool dma_low_feed(dma_low_t *l, uint32_t min, uint32_t free_now, uint64_t at_ms, uint32_t big, char const *task);

// Lines for "fw mem", one per call: i 0 is the first sample, 1.. the kept drops oldest first. 0 when i is past the end.
// "mem dma_low first min 90000 fell 0 at 12.345s free 99000 big 0 task -"
// "mem dma_low drop 3/7 min 68423 fell 21577 at 3600.250s free 70000 big 1556 task ota"
size_t dma_low_line(dma_low_t const *l, unsigned i, char *out, size_t cap);
