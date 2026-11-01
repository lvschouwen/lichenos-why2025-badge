#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Where the PSRAM heap is (#89): the pools the IDF registered with the heap, as [lo, hi) ranges. Pure, host-tested in
// host_tests/psram_heap_test.c; psram_guard.c records the pools and asks before every LVGL free().

typedef struct {
    uintptr_t lo, hi; // [lo, hi)
} psram_range_t;

// Is p inside one of the pools?
bool psram_heap_has(psram_range_t const *pools, size_t n, uintptr_t p);

// The lowest bit whose flip puts p inside a pool, or -1: a pointer one bit away from a heap address is what a flipped
// PSRAM bit leaves behind, a pointer with no such neighbour points at a software bug
int psram_heap_flip_bit(psram_range_t const *pools, size_t n, uintptr_t p);
