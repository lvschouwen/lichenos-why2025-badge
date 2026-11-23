#include "panic_ring.h"

#include <string.h>

bool panic_ring_has(panic_ring_t const *r, uint32_t ctx) {
    for (size_t i = 0; ctx && i < PANIC_RING_SIZE; i++) {
        if (r->key[i] == ctx) {
            return true;
        }
    }
    return false;
}

// The entry a new context takes: an empty one, else the first unpinned one from next on, else the one at next
static size_t victim(panic_ring_t const *r) {
    size_t first = r->next % PANIC_RING_SIZE;
    for (size_t k = 0; k < PANIC_RING_SIZE; k++) {
        size_t i = (first + k) % PANIC_RING_SIZE;
        if (!r->key[i]) {
            return i;
        }
    }
    for (size_t k = 0; k < PANIC_RING_SIZE; k++) {
        size_t i = (first + k) % PANIC_RING_SIZE;
        if (!r->pinned[i]) {
            return i;
        }
    }
    return first;
}

void panic_ring_note(panic_ring_t *r, uint32_t ctx, char const *id, bool pin) {
    if (!ctx || !id) {
        return;
    }
    for (size_t i = 0; pin && i < PANIC_RING_SIZE; i++) {
        if (r->key[i] == ctx) {
            r->pinned[i] = 1;
        }
    }
    if (panic_ring_has(r, ctx)) {
        return;
    }
    size_t i = victim(r);
    r->next  = (uint8_t)((i + 1) % PANIC_RING_SIZE);
    __atomic_store_n(&r->key[i], 0u, __ATOMIC_RELEASE); // not named while the id is half written
    r->pinned[i] = pin ? 1 : 0;
    size_t n = strlen(id);
    n        = n < PANIC_RING_ID_CAP ? n : PANIC_RING_ID_CAP - 1;
    memcpy(r->id[i], id, n);
    r->id[i][n] = '\0';
    __atomic_store_n(&r->key[i], ctx, __ATOMIC_RELEASE);
}

void panic_ring_drop(panic_ring_t *r, uint32_t ctx) {
    for (size_t i = 0; ctx && i < PANIC_RING_SIZE; i++) {
        if (r->key[i] == ctx) {
            __atomic_store_n(&r->key[i], 0u, __ATOMIC_RELEASE);
            r->id[i][0] = '\0';
            r->pinned[i] = 0;
        }
    }
}
