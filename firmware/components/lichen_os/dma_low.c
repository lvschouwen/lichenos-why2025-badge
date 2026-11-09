#include "dma_low.h"

#include <stdio.h>
#include <string.h>

static void mark_set(dma_low_mark_t *m, uint32_t min, uint32_t fell, uint32_t free_now, uint64_t at_ms, char const *task) {
    m->at_ms    = at_ms;
    m->min      = min;
    m->fell     = fell;
    m->free_now = free_now;
    snprintf(m->task, sizeof(m->task), "%s", task && task[0] ? task : "-");
}

static dma_low_mark_t const *newest(dma_low_t const *l) {
    return l->drops ? &l->mark[(l->drops - 1) % DMA_LOW_KEEP] : &l->first;
}

bool dma_low_feed(dma_low_t *l, uint32_t min, uint32_t free_now, uint64_t at_ms, char const *task) {
    if (!l->started) {
        mark_set(&l->first, min, 0, free_now, at_ms, task);
        l->started = true;
        return true;
    }
    // Against the last record, not the last sample: a peak built up in small steps still counts once it adds up
    uint32_t last = newest(l)->min;
    if (min >= last || last - min < DMA_LOW_STEP) {
        return false;
    }
    mark_set(&l->mark[l->drops % DMA_LOW_KEEP], min, last - min, free_now, at_ms, task);
    l->drops++;
    return true;
}

static size_t put(dma_low_mark_t const *m, char *out, size_t cap, char const *what) {
    int n = snprintf(out, cap, "mem dma_low %s min %u fell %u at %llu.%03us free %u task %s", what, (unsigned)m->min,
                     (unsigned)m->fell, (unsigned long long)(m->at_ms / 1000), (unsigned)(m->at_ms % 1000),
                     (unsigned)m->free_now, m->task);
    return n < 0 ? 0 : (size_t)n;
}

size_t dma_low_line(dma_low_t const *l, unsigned i, char *out, size_t cap) {
    if (!l->started || cap == 0) {
        return 0;
    }
    if (i == 0) {
        return put(&l->first, out, cap, "first");
    }
    uint32_t kept = l->drops < DMA_LOW_KEEP ? l->drops : DMA_LOW_KEEP;
    if (i > kept) {
        return 0;
    }
    uint32_t nth = l->drops - kept + i; // 1-based number of this drop among all of them
    char     what[32];
    snprintf(what, sizeof(what), "drop %u/%u", (unsigned)nth, (unsigned)l->drops);
    return put(&l->mark[(nth - 1) % DMA_LOW_KEEP], out, cap, what);
}
