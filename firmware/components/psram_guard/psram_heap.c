#include "psram_heap.h"

bool psram_heap_has(psram_range_t const *pools, size_t n, uintptr_t p) {
    for (size_t i = 0; i < n; i++) {
        if (p >= pools[i].lo && p < pools[i].hi) {
            return true;
        }
    }
    return false;
}

int psram_heap_flip_bit(psram_range_t const *pools, size_t n, uintptr_t p) {
    for (int b = 0; b < (int)(8 * sizeof(uintptr_t)); b++) {
        if (psram_heap_has(pools, n, p ^ ((uintptr_t)1 << b))) {
            return b;
        }
    }
    return -1;
}
