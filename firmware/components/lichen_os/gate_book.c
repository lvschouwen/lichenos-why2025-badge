// The gate's check of a caller's context before the books (#230 E1b), see gate_book.h. Pure: no ESP-IDF here.
#include "gate_book.h"

#include <stddef.h>

bool gate_owner(gate_env_t const *g, uint32_t ctx, char const *what, gate_owner_t *who) {
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

mem_charge_t gate_mem_charge(gate_env_t const *g, mem_book_t *b, uint32_t ctx, bool internal, uint32_t bytes,
                             gate_owner_t *who) {
    if (!gate_owner(g, ctx, "lichen_alloc", who)) {
        return MEM_GATE_REFUSED;
    }
    g->take();
    mem_charge_t charge = mem_book_charge(b, who->ctx, who->label, who->os, internal, bytes);
    g->give();
    return charge;
}

mem_release_t gate_mem_release(gate_env_t const *g, mem_book_t *b, uint32_t ctx, void const *ptr, char const **label,
                               uint32_t *bytes) {
    gate_owner_t who;
    if (!gate_owner(g, ctx, "lichen_free", &who)) {
        return MEM_GATE_RELEASE_REFUSED; // before ptr is looked at: a made-up ctx may come with a made-up pointer
    }
    bool internal = false;
    g->block(ptr, &internal, bytes);
    *label = who.label;
    g->take();
    mem_release_t rel = mem_book_release(b, who.ctx, internal, *bytes);
    g->give();
    return rel;
}

// #538: an SD app's limits. stack_max is the most one stack may be, budget the most the ctx's internal stacks may add up to
// (bytes, 0 = none). Only internal stacks count: the budget is the manifest's internal_ram_kib.
static int task_limit(task_book_t const *b, uint32_t ctx, task_spec_t const *spec) {
    if (spec->stack_max && spec->stack > spec->stack_max) {
        return TASK_GATE_STACK;
    }
    if (!spec->budget || spec->mem != TASK_STACK_INTERNAL) {
        return 0;
    }
    uint64_t sum = spec->stack;
    for (int i = 0; i < TASK_BOOK_SLOTS; i++) {
        task_slot_t const *s = &b->slot[i];
        if (s->used && s->ctx == ctx && s->mem == TASK_STACK_INTERNAL) {
            sum += s->stack;
        }
    }
    return sum > spec->budget ? TASK_GATE_BUDGET : 0;
}

int gate_task_add(gate_env_t const *g, task_book_t *b, uint32_t ctx, task_spec_t const *spec, gate_owner_t *who) {
    if (!gate_owner(g, ctx, "lichen_task_start", who)) {
        return TASK_GATE_REFUSED;
    }
    task_owner_t owner = {.ctx = who->ctx, .label = who->label};
    g->take();
    int lim = task_limit(b, who->ctx, spec);
    if (lim) { // under the same lock as the add: two starts cannot both pass
        g->give();
        return lim;
    }
    int slot = task_book_add(b, owner, spec->name, spec->stack, spec->cls, spec->mem, spec->fn, spec->arg);
    g->give();
    return slot;
}

void gate_task_run(gate_env_t const *g, uint32_t ctx, void (*fn)(void *arg), void *arg) {
    (void)g->enter(ctx); // for good: the task ends with fn, so there is nothing to leave to
    fn(arg);
}
