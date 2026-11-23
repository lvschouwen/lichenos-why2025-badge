#pragma once

// The crash hook's names for apps (#232, #520): which app a context stands for, read with nothing but internal RAM,
// because the hook may be running because PSRAM faulted and the app table lives there. A small ring, not one entry per
// registered app: it holds the apps that ran lately (lichen_ctx_enter and a task's start note them), so a hundred
// registered apps cost the same eight entries. The limit is deliberate: when more than eight apps run on their own or the
// hooks of many apps interleave, an app the ring evicted that then crashes is not named (nobody is blamed). A crash in a context the ring no longer holds blames nobody (the safe
// mode rule only counts crashes it can name). Pure (no ESP-IDF), host-tested in host_tests/panic_ring_test.c.
//
// note and drop run on ordinary tasks (the caller serialises them: lichen_app.c holds a spinlock, which is fine as the
// ring is internal); panic_ring_find is read by the crash hook with no lock, so a note writes the key last.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANIC_RING_SIZE 8
#define PANIC_RING_ID_CAP 14 // an app id with its NUL (app_ns.h), LICHEN_PANIC_ID_CAP

typedef struct {
    uint32_t key[PANIC_RING_SIZE]; // the lichen_ctx_t (slot + generation); 0 = empty
    char     id[PANIC_RING_SIZE][PANIC_RING_ID_CAP];
    uint8_t  pinned[PANIC_RING_SIZE]; // 1: the app runs on its own, hooks entering other contexts leave it be
    uint8_t  next;                    // where the search for the entry a new context replaces starts
} panic_ring_t;

// ctx's app is named id from now on (ctx not 0, id not NULL). pin: the app runs on its own (it started a task), so
// contexts that merely entered for a hook never replace it, however many do; only another pinned one can, when every
// entry is pinned. A context already in the ring keeps its place (and becomes pinned when pin says so); a new one takes
// an empty entry, else the oldest unpinned, else the oldest.
void panic_ring_note(panic_ring_t *r, uint32_t ctx, char const *id, bool pin);
// Forgets ctx (its app left): the hook stops naming it
void panic_ring_drop(panic_ring_t *r, uint32_t ctx);
// Whether ctx is in the ring
bool panic_ring_has(panic_ring_t const *r, uint32_t ctx);

// The name of ctx, NULL when the ring has none. Inline: the crash hook calls it from IRAM and may not call flash code.
static inline __attribute__((always_inline)) char const *panic_ring_find(panic_ring_t const *r, uint32_t ctx) {
    for (size_t i = 0; ctx && i < PANIC_RING_SIZE; i++) {
        if (__atomic_load_n(&r->key[i], __ATOMIC_ACQUIRE) == ctx && r->id[i][0]) {
            return r->id[i];
        }
    }
    return NULL;
}
