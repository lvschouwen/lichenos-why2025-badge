#include "raw_hooks.h"

#include <stddef.h>

// FREE -> CLAIMING (an adder owns fn/arg) -> LIVE (runnable) -> REMOVING (not run any more; the remover waits for the
// calls in flight) -> FREE. Only the adder that won FREE -> CLAIMING writes fn/arg, so they cannot change under a
// running call or a waiting remover.
typedef enum { RAW_HOOK_FREE = 0, RAW_HOOK_CLAIMING, RAW_HOOK_LIVE, RAW_HOOK_REMOVING } raw_hook_state_t;

bool raw_hooks_add(raw_hooks_t *h, key_raw_fn fn, void *arg) {
    for (size_t i = 0; i < RAW_HOOKS_MAX; i++) {
        raw_hook_slot_t *s    = &h->slot[i];
        int              free = RAW_HOOK_FREE;
        if (atomic_compare_exchange_strong(&s->state, &free, RAW_HOOK_CLAIMING)) {
            atomic_store_explicit(&s->fn, fn, memory_order_relaxed);
            atomic_store_explicit(&s->arg, arg, memory_order_relaxed);
            atomic_store(&s->state, RAW_HOOK_LIVE); // publishes fn/arg
            return true;
        }
    }
    return false;
}

static bool holds(raw_hook_slot_t *s, key_raw_fn fn, void *arg) {
    return atomic_load_explicit(&s->fn, memory_order_relaxed) == fn &&
           atomic_load_explicit(&s->arg, memory_order_relaxed) == arg;
}

// Takes s out of the table if it holds (fn, arg) and is live; true = this caller did
static bool unpublish(raw_hook_slot_t *s, key_raw_fn fn, void *arg) {
    int live = RAW_HOOK_LIVE;
    if (atomic_load(&s->state) != RAW_HOOK_LIVE || !holds(s, fn, arg) ||
        !atomic_compare_exchange_strong(&s->state, &live, RAW_HOOK_REMOVING)) {
        return false;
    }
    if (!holds(s, fn, arg)) {
        // The slot was freed and taken by another hook between the look and the exchange: put that one back
        atomic_store(&s->state, RAW_HOOK_LIVE);
        return false;
    }
    return true;
}

void raw_hooks_remove(raw_hooks_t *h, key_raw_fn fn, void *arg, void (*wait)(void)) {
    for (size_t i = 0; i < RAW_HOOKS_MAX; i++) {
        raw_hook_slot_t *s = &h->slot[i];
        if (!unpublish(s, fn, arg)) {
            continue;
        }
        // Sequentially consistent on both sides: either the run saw REMOVING (and calls nothing), or its inflight
        // increment is visible here and this waits for it
        while (wait && atomic_load(&s->inflight) > 0) {
            wait();
        }
        atomic_store(&s->state, RAW_HOOK_FREE);
    }
}

bool raw_hooks_run(raw_hooks_t *h, key_raw_event_t const *ev) {
    bool swallowed = false;
    for (size_t i = 0; i < RAW_HOOKS_MAX; i++) {
        raw_hook_slot_t *s = &h->slot[i];
        atomic_fetch_add(&s->inflight, 1); // before the state check: see raw_hooks_remove
        if (atomic_load(&s->state) == RAW_HOOK_LIVE) {
            key_raw_fn fn  = atomic_load_explicit(&s->fn, memory_order_relaxed);
            void      *arg = atomic_load_explicit(&s->arg, memory_order_relaxed);
            if (fn(ev, arg)) {
                swallowed = true;
            }
        }
        atomic_fetch_sub(&s->inflight, 1);
    }
    return swallowed;
}
