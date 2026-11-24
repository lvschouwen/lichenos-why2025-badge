// Bench-only (#536): blocks made through the OS's malloc shims (export_shims.c) that are never freed, so the loader's leftover
// sweep at unload has the shim path to free as well as the lichen_alloc one (app.c). The one file of an SD app that may call
// malloc: tools/layers.conf names it in `sdapp-allow` (the boundary check forbids malloc in apps otherwise, they use
// lichen_alloc). Never copy this into a real app.
#include "leak.h"

#include <stdlib.h>

#define SHIM_BLOCKS 2
#define SHIM_BYTES  4096

static void *volatile g_shim_leaked[SHIM_BLOCKS]; // kept only so the compiler cannot drop the allocations

void bench_leak_shim_blocks(void) {
    for (int i = 0; i < SHIM_BLOCKS; i++) {
        g_shim_leaked[i] = malloc(SHIM_BYTES);
    }
}
