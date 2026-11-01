// Host test: PSRAM heap membership for the free() guard (#89, #93)
#include "psram_heap.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    // Two pools, as the IDF registers them around the XIP segments: the heap is these, nothing else
    psram_range_t pools[] = {{0x481C0000u, 0x48A00000u}, {0x48A10000u, 0x4A000000u}};
    CHECK(psram_heap_has(pools, 2, 0x48C15AC0u));
    CHECK(psram_heap_has(pools, 2, 0x481C0000u));  // first byte of a pool
    CHECK(!psram_heap_has(pools, 2, 0x48A05000u)); // between the pools
    CHECK(!psram_heap_has(pools, 2, 0x4FF5A000u)); // internal RAM
    CHECK(!psram_heap_has(pools, 2, 0x4A000000u)); // one past the last pool
    CHECK(!psram_heap_has(pools, 0, 0x48C15AC0u)); // no pools known: nothing is in the heap

    CHECK(psram_heap_flip_bit(pools, 2, 0x08C15AC0u) == 30); // bit 30 lost: one flip back into the heap
    CHECK(psram_heap_flip_bit(pools, 2, 0x48C15EC0u) == 0);  // already in: the lowest bit is reported
    CHECK(psram_heap_flip_bit(pools, 2, 0x48A05000u) == 16); // the gap is 64 KB wide: bits 0..15 stay in it, bit 16 lands in a pool
    CHECK(psram_heap_flip_bit(pools, 2, 0x00000056u) == -1); // garbage, no single-bit neighbour
    CHECK(psram_heap_flip_bit(pools, 0, 0x48C15AC0u) == -1);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_heap_test ok\n");
    return 0;
}
