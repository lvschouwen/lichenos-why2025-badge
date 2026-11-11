// The gate's check of a caller's context before the books (#230 E1b), see gate_book.h. Pure: no ESP-IDF here.
#include "gate_book.h"

#include <stddef.h>

bool gate_owner(gate_ctx_t const *g, uint32_t ctx, char const *what, gate_owner_t *who) {
    char const *id   = NULL;
    char const *name = NULL;
    if (!g->resolve(ctx, &id, &name, what)) {
        return false;
    }
    bool        os    = ctx == g->os(); // by ctx: an app without an id is still an app, with its quota
    char const *label = id ? id : name ? name : "?";
    *who              = (gate_owner_t){.ctx = ctx, .label = os ? NULL : label, .os = os};
    return true;
}

mem_charge_t gate_mem_charge(gate_ctx_t const *g, gate_lock_t const *l, mem_book_t *b, uint32_t ctx, bool internal,
                             uint32_t bytes, gate_owner_t *who) {
    if (!gate_owner(g, ctx, "lichen_alloc", who)) {
        return MEM_GATE_REFUSED;
    }
    l->take();
    mem_charge_t charge = mem_book_charge(b, who->ctx, who->label, who->os, internal, bytes);
    l->give();
    return charge;
}

mem_release_t gate_mem_release(gate_ctx_t const *g, gate_lock_t const *l, mem_book_t *b, uint32_t ctx, bool internal,
                               uint32_t bytes, char const **label) {
    gate_owner_t who;
    if (!gate_owner(g, ctx, "lichen_free", &who)) {
        return MEM_GATE_RELEASE_REFUSED;
    }
    *label = who.label;
    l->take();
    mem_release_t rel = mem_book_release(b, who.ctx, internal, bytes);
    l->give();
    return rel;
}

int gate_task_add(gate_ctx_t const *g, gate_lock_t const *l, task_book_t *b, uint32_t ctx, task_spec_t const *spec,
                  gate_owner_t *who) {
    if (!gate_owner(g, ctx, "lichen_task_start", who)) {
        return TASK_GATE_REFUSED;
    }
    task_owner_t owner = {.ctx = who->ctx, .label = who->label};
    l->take();
    int slot = task_book_add(b, owner, spec->name, spec->stack, spec->cls, spec->mem, spec->fn, spec->arg);
    l->give();
    return slot;
}

void gate_task_run(gate_ctx_t const *g, uint32_t ctx, void (*fn)(void *arg), void *arg) {
    (void)g->enter(ctx); // for good: the task ends with fn, so there is nothing to leave to
    fn(arg);
}
