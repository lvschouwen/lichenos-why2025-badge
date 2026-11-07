#include "mem_fmt.h"

#include <stdio.h>

// Whole percent, rounded down, never above 100 (a min below free can't exceed total, but the inputs are not trusted)
static unsigned pct(uint32_t part, uint32_t whole) {
    if (whole == 0) {
        return 0;
    }
    uint64_t v = (uint64_t)part * 100 / whole;
    return v > 100 ? 100 : (unsigned)v;
}

size_t mem_fmt_heap(char *out, size_t cap, char const *name, uint32_t free_b, uint32_t largest, uint32_t min_free,
                    uint32_t total) {
    unsigned used = total > free_b ? pct(total - free_b, total) : 0;
    int      n    = snprintf(out, cap, "mem %s free %u largest %u min %u total %u free_pct %u min_pct %u largest_pct %u used_pct %u",
                             name, (unsigned)free_b, (unsigned)largest, (unsigned)min_free, (unsigned)total, pct(free_b, total),
                             pct(min_free, total), pct(largest, free_b), used);
    return n < 0 ? 0 : (size_t)n;
}
