// lichen_timer's bookkeeping (#231), see timer_book.h. Pure.
#include "timer_book.h"

#include <string.h>

_Static_assert(TIMER_BOOK_SLOTS < (1u << TIMER_BOOK_SLOT_BITS), "a slot + 1 fits the handle's low bits");

#define GEN_MAX ((1u << (32u - TIMER_BOOK_SLOT_BITS)) - 1u)

static int slot_of(uint32_t handle) {
    uint32_t s = handle & ((1u << TIMER_BOOK_SLOT_BITS) - 1u);
    return s >= 1 && s <= TIMER_BOOK_SLOTS ? (int)s - 1 : -1;
}

uint32_t timer_book_add(timer_book_t *b, uint32_t ctx, void (*cb)(void *arg), void *arg) {
    if (!ctx || !cb) {
        return 0;
    }
    for (int i = 0; i < TIMER_BOOK_SLOTS; i++) {
        timer_slot_t *s = &b->slot[i];
        if (s->handle) {
            continue;
        }
        b->gen    = b->gen >= GEN_MAX ? 1u : b->gen + 1u; // 2^27 timers before a generation comes back
        s->handle = (b->gen << TIMER_BOOK_SLOT_BITS) | (uint32_t)(i + 1);
        s->ctx    = ctx;
        s->cb     = cb;
        s->arg    = arg;
        return s->handle;
    }
    return 0;
}

timer_slot_t const *timer_book_find(timer_book_t const *b, uint32_t handle) {
    int i = handle ? slot_of(handle) : -1;
    return i >= 0 && b->slot[i].handle == handle ? &b->slot[i] : NULL;
}

bool timer_book_remove(timer_book_t *b, uint32_t handle) {
    int i = handle ? slot_of(handle) : -1;
    if (i < 0 || b->slot[i].handle != handle) {
        return false;
    }
    memset(&b->slot[i], 0, sizeof(b->slot[i]));
    return true;
}

size_t timer_book_count(timer_book_t const *b, uint32_t ctx) {
    size_t n = 0;
    for (int i = 0; ctx && i < TIMER_BOOK_SLOTS; i++) {
        n += b->slot[i].handle && b->slot[i].ctx == ctx;
    }
    return n;
}

uint32_t timer_book_next(timer_book_t const *b, uint32_t ctx, size_t *from) {
    for (size_t i = *from; ctx && i < TIMER_BOOK_SLOTS; i++) {
        if (b->slot[i].handle && b->slot[i].ctx == ctx) {
            *from = i + 1;
            return b->slot[i].handle;
        }
    }
    *from = TIMER_BOOK_SLOTS;
    return 0;
}
