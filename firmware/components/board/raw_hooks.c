#include "raw_hooks.h"

#include <stddef.h>

// A slot's state word: the state in the low two bits, a generation above it that every claim bumps. The state goes
// FREE -> CLAIMING (one adder owns fn/arg) -> LIVE (runnable) -> REMOVING (not run any more; the remover waits for the
// calls in flight) -> FREE. Only the adder that won FREE -> CLAIMING writes fn/arg, so they cannot change under a
// running call or a waiting remover. The generation makes a remover's LIVE -> REMOVING exchange fail when the slot was
// freed and claimed again since it read fn/arg (ABA), so it never takes out another owner's hook.
enum { RAW_HOOK_FREE = 0, RAW_HOOK_CLAIMING, RAW_HOOK_LIVE, RAW_HOOK_REMOVING, RAW_HOOK_STATE_MASK = 3u };

static unsigned state_of(unsigned word) {
    return word & RAW_HOOK_STATE_MASK;
}

static unsigned with_state(unsigned word, unsigned state) {
    return (word & ~RAW_HOOK_STATE_MASK) | state;
}

bool raw_hooks_add(raw_hooks_t *h, key_raw_fn fn, void *arg) {
    if (!fn) {
        return false; // a live slot without a function would be called on the keyboard task
    }
    for (size_t i = 0; i < RAW_HOOKS_MAX; i++) {
        raw_hook_slot_t *s       = &h->slot[i];
        unsigned         seen    = atomic_load(&s->word);
        unsigned         claimed = with_state(seen + RAW_HOOK_STATE_MASK + 1u, RAW_HOOK_CLAIMING); // next generation
        if (state_of(seen) == RAW_HOOK_FREE && atomic_compare_exchange_strong(&s->word, &seen, claimed)) {
            atomic_store_explicit(&s->fn, fn, memory_order_relaxed);
            atomic_store_explicit(&s->arg, arg, memory_order_relaxed);
            atomic_store(&s->word, with_state(claimed, RAW_HOOK_LIVE)); // publishes fn/arg
            return true;
        }
    }
    return false;
}

// Takes s out of the table if it holds (fn, arg) and is live; true = this caller did
static bool unpublish(raw_hook_slot_t *s, key_raw_fn fn, void *arg) {
    unsigned seen = atomic_load(&s->word);
    if (state_of(seen) != RAW_HOOK_LIVE || atomic_load_explicit(&s->fn, memory_order_relaxed) != fn ||
        atomic_load_explicit(&s->arg, memory_order_relaxed) != arg) {
        return false;
    }
    // Same generation still: no claim since the reads above, so they were this live hook's
    return atomic_compare_exchange_strong(&s->word, &seen, with_state(seen, RAW_HOOK_REMOVING));
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
        atomic_store(&s->word, with_state(atomic_load(&s->word), RAW_HOOK_FREE));
    }
}

bool raw_hooks_run(raw_hooks_t *h, key_raw_event_t const *ev) {
    bool swallowed = false;
    for (size_t i = 0; i < RAW_HOOKS_MAX; i++) {
        raw_hook_slot_t *s = &h->slot[i];
        atomic_fetch_add(&s->inflight, 1); // before the state check: see raw_hooks_remove
        if (state_of(atomic_load(&s->word)) == RAW_HOOK_LIVE) {
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
