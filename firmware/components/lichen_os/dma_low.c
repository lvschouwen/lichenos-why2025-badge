#include "dma_low.h"

#include <stdio.h>

static void take_big(dma_low_mark_t *m, uint32_t big, char const *task) {
    m->big = big;
    snprintf(m->task, sizeof(m->task), "%s", task && task[0] ? task : "-");
}

static dma_low_mark_t mark_of(uint32_t min, uint32_t fell, uint32_t free_now, uint64_t at_ms, uint32_t big,
                              char const *task) {
    dma_low_mark_t m = {.at_ms = at_ms, .min = min, .fell = fell, .free_now = free_now};
    take_big(&m, big, task);
    return m;
}

static dma_low_mark_t *newest(dma_low_t *l) {
    return l->drops ? &l->mark[(l->drops - 1) % DMA_LOW_KEEP] : &l->first;
}

// The record's figures follow the heap down; ref stays where the record started, so a peak built from many small
// falls (one per sample) still starts a new record once it adds up to a step
bool dma_low_feed(dma_low_t *l, uint32_t min, uint32_t free_now, uint64_t at_ms, uint32_t big, char const *task) {
    if (!l->started) {
        l->first   = mark_of(min, 0, free_now, at_ms, big, task);
        l->ref     = min;
        l->started = true;
        return true;
    }
    dma_low_mark_t *last = newest(l);
    if (min >= last->min) {
        return false; // the heap's mark never rises; a higher figure is noise from the caller
    }
    uint32_t fell = last->min - min;
    if (l->ref - min < DMA_LOW_STEP) {
        // Churn: the newest record keeps its moment, follows the true minimum (the "fw mem dma min" figure) and
        // keeps the largest allocation seen while it fell
        last->fell += fell;
        last->min = min;
        if (big > last->big) {
            take_big(last, big, task);
        }
        return false;
    }
    l->mark[l->drops % DMA_LOW_KEEP] = mark_of(min, fell, free_now, at_ms, big, task);
    l->drops++;
    l->ref = min;
    return true;
}

static size_t put(dma_low_mark_t const *m, char *out, size_t cap, char const *what) {
    int n = snprintf(out, cap, "mem dma_low %s min %u fell %u at %llu.%03us free %u big %u task %s", what,
                     (unsigned)m->min, (unsigned)m->fell, (unsigned long long)(m->at_ms / 1000),
                     (unsigned)(m->at_ms % 1000), (unsigned)m->free_now, (unsigned)m->big, m->task);
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
