#pragma once
// The log line of the unload sweep of the blocks an SD app took with lichen_alloc and left (#557); the sweep itself is
// ptr_map_sweep over the loader's second map. Pure (host_tests/ptr_map_test.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Writes "<id>: freed N booked blocks (B B)[, not freed: untracked frees]" to buf. False (buf untouched) when there is
// nothing to say: no block freed and no untracked free.
bool booked_sweep_line(char *buf, size_t cap, char const *id, uint32_t blocks, uint32_t bytes, bool unsafe);
