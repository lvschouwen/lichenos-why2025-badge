// The gate's own heap, forwarded to the OS's lichen_alloc (#296): see gate_mem.h
#include "gate_mem.h"

#include "lichen_mem.h"
#include "lichen_sys_mem.h"

#include <stdatomic.h>
#include <string.h>

// lichen_os sets it on the boot task before the first screen; published with release and read with acquire, as
// lichen_ui_ops.c does, so a reader that sees the table sees its members too
static _Atomic(lichen_sys_mem_ops_t const *) s_ops;

void lichen_sys_mem_ops_set(lichen_sys_mem_ops_t const *ops) {
    atomic_store_explicit(&s_ops, ops, memory_order_release);
}

static lichen_sys_mem_ops_t const *ops(void) {
    return atomic_load_explicit(&s_ops, memory_order_acquire);
}

// Rows and filters are read by the CPU only: PSRAM, which keeps them out of the internal RAM DMA needs
static void *alloc_for(lichen_ctx_t owner, size_t size) {
    lichen_sys_mem_ops_t const *o = ops();
    return o && o->alloc ? o->alloc(owner, size, LICHEN_MEM_BULK | LICHEN_MEM_ZERO) : NULL;
}

void *gate_mem_new(size_t size, lichen_ctx_t *owner) {
    lichen_ctx_t self = lichen_ctx_self();
    void        *p    = alloc_for(self, size);
    if (p) {
        *owner = self;
    }
    return p;
}

void gate_mem_free(lichen_ctx_t owner, void *ptr) {
    lichen_sys_mem_ops_t const *o = ops();
    if (ptr && o && o->release) { // a block exists only if the table did: it is never taken away on the badge
        o->release(owner, ptr);
    }
}

char *gate_mem_strdup(lichen_ctx_t owner, char const *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s);
    if (n > GATE_MEM_TEXT_MAX) {
        n = GATE_MEM_TEXT_MAX;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
            n--; // s[n] is the first byte left out: a continuation byte means its character would be cut, so it goes
        }
    }
    char *copy = alloc_for(owner, n + 1); // zeroed: the terminator is there already
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

bool gate_mem_pair_set(gate_mem_pair_t *pair, char const *a, char const *b) {
    lichen_ctx_t owner = lichen_ctx_self();
    char        *na    = gate_mem_strdup(owner, a);
    char        *nb    = gate_mem_strdup(owner, b);
    if ((a && !na) || (b && !nb)) {
        gate_mem_free(owner, na);
        gate_mem_free(owner, nb);
        return false;
    }
    gate_mem_pair_clear(pair);
    pair->owner = owner;
    pair->a     = na;
    pair->b     = nb;
    return true;
}

void gate_mem_pair_clear(gate_mem_pair_t *pair) {
    gate_mem_free(pair->owner, pair->a);
    gate_mem_free(pair->owner, pair->b);
    pair->a = NULL;
    pair->b = NULL;
}
