#pragma once
// The log line of the unload sweep of the blocks an SD app took with lichen_alloc and left (#557); the sweep itself is
// ptr_map_sweep over the loader's second map. Pure (host_tests/ptr_map_test.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Writes "<id>: freed N booked blocks (B B)[, not freed: untracked frees]" to buf. False (buf untouched) when there is
// nothing to say: no block freed and no untracked free.
bool booked_sweep_line(char *buf, size_t cap, char const *id, uint32_t blocks, uint32_t bytes, bool unsafe);

// Freeing one swept block exactly once (#557). A block an app took through the malloc shims is in both of the loader's maps,
// so whichever way it is freed (under the app's context, or straight to the heap when the context is stale after a card
// removal) it is first forgotten from both: the other sweep must not find it and free it a second time.
typedef struct {
    void *user;
    void (*forget_blocks)(void *user, void *p); // the malloc map
    void (*forget_booked)(void *user, void *p); // the lichen_alloc map
    void (*free_stale)(void *user, void *p);    // the heap, the context's row is gone
    void (*free_owned)(void *user, void *p);    // lichen_free under the app's context
} booked_release_env_t;
void booked_release(booked_release_env_t const *env, void *p, bool stale);
