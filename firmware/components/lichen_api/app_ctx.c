#include "app_ctx.h"

#include "gate_app.h"

#include <stdlib.h>
#include <string.h>

static uint32_t gen_of(uint32_t draw) {
    uint32_t gen = draw & APP_CTX_GEN_MASK;
    return gen ? gen : 1u; // 0 marks a free slot
}

static uint32_t make_ctx(uint32_t slot, uint32_t gen) {
    return (gen << APP_CTX_SLOT_BITS) | slot;
}

static uint32_t slot_of(uint32_t ctx) {
    return ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
}

static void *mem_alloc(app_ctx_table_t const *t, size_t size) {
    return t->mem.alloc ? t->mem.alloc(size) : calloc(1, size);
}

static void view_lock(app_ctx_table_t const *t) {
    if (t->mem.lock) {
        t->mem.lock();
    }
}

static void view_unlock(app_ctx_table_t const *t) {
    if (t->mem.unlock) {
        t->mem.unlock();
    }
}

static void mem_release(app_ctx_table_t const *t, void *p) {
    if (t->mem.release) {
        t->mem.release(p);
    } else {
        free(p);
    }
}

// The chunk holding slot (or, for the dense view, index) n, NULL when none was allocated yet. The pointer is published
// once and never changes.
static app_ctx_chunk_t *chunk_of(app_ctx_table_t const *t, uint32_t n) {
    return __atomic_load_n(&t->chunk[n / APP_CTX_CHUNK], __ATOMIC_ACQUIRE);
}

static uint32_t gen_at(app_ctx_table_t const *t, uint32_t slot) {
    app_ctx_chunk_t *c = chunk_of(t, slot);
    return c ? __atomic_load_n(&c->gen[slot % APP_CTX_CHUNK], __ATOMIC_ACQUIRE) : 0u;
}

void app_ctx_init_with(app_ctx_table_t *t, uint32_t draw, app_ctx_mem_t const *mem, size_t limit, size_t state_size) {
    memset(t, 0, sizeof(*t));
    t->state_size = state_size;
    if (mem) {
        t->mem = *mem;
    }
    t->limit  = (uint16_t)(limit < APP_CTX_MAX_APPS ? limit : APP_CTX_MAX_APPS);
    t->os_gen = gen_of(draw);
}

void app_ctx_init(app_ctx_table_t *t, uint32_t draw) {
    app_ctx_init_with(t, draw, NULL, APP_CTX_MAX_APPS, 0);
}

void app_ctx_destroy(app_ctx_table_t *t) {
    for (size_t i = 0; i < APP_CTX_CHUNKS; i++) {
        if (t->chunk[i]) {
            for (size_t k = 0; k < APP_CTX_CHUNK; k++) {
                if (t->chunk[i]->state[k]) {
                    mem_release(t, t->chunk[i]->state[k]);
                }
            }
            mem_release(t, t->chunk[i]);
            t->chunk[i] = NULL;
        }
    }
    t->os_gen = 0;
}

bool app_ctx_ready(app_ctx_table_t const *t) {
    return t->os_gen != 0;
}

// The lowest free slot, 0 for none (every slot up to the limit is taken)
static uint32_t free_slot(app_ctx_table_t const *t) {
    for (uint32_t slot = 1; slot <= t->limit; slot++) {
        if (gen_at(t, slot) == 0) {
            return slot;
        }
    }
    return 0;
}

// Makes sure the chunk holding n exists; false when it cannot be had
static bool chunk_ensure(app_ctx_table_t *t, uint32_t n) {
    if (chunk_of(t, n)) {
        return true;
    }
    app_ctx_chunk_t *c = mem_alloc(t, sizeof(*c));
    if (!c) {
        return false;
    }
    __atomic_store_n(&t->chunk[n / APP_CTX_CHUNK], c, __ATOMIC_RELEASE);
    return true;
}

// The dense view (index -> slot) is kept in the chunks' order bytes, index n in chunk n / 16
static void order_set(app_ctx_table_t *t, size_t index, uint32_t slot) {
    chunk_of(t, (uint32_t)index)->order[index % APP_CTX_CHUNK] = (uint8_t)slot;
}

static uint32_t order_get(app_ctx_table_t const *t, size_t index) {
    return chunk_of(t, (uint32_t)index)->order[index % APP_CTX_CHUNK];
}

uint32_t app_ctx_add(app_ctx_table_t *t, void const *owner, uint32_t draw) {
    if (!owner || !app_ctx_ready(t) || t->apps >= t->limit) {
        return APP_CTX_NONE;
    }
    uint32_t slot = free_slot(t);
    if (slot == 0 || !chunk_ensure(t, slot) || !chunk_ensure(t, t->apps)) {
        return APP_CTX_NONE;
    }
    void *state = t->state_size ? mem_alloc(t, t->state_size) : NULL;
    if (t->state_size && !state) {
        return APP_CTX_NONE;
    }
    app_ctx_chunk_t *c    = chunk_of(t, slot);
    uint32_t         last = c->last[slot % APP_CTX_CHUNK];
    uint32_t         gen  = gen_of(draw);
    if (gen == last) {
        gen = gen_of(gen + 1u); // a reused slot never repeats its previous generation
    }
    view_lock(t);
    order_set(t, t->apps, slot);
    __atomic_store_n(&c->owner[slot % APP_CTX_CHUNK], owner, __ATOMIC_RELAXED);
    __atomic_store_n(&c->state[slot % APP_CTX_CHUNK], state, __ATOMIC_RELAXED);
    __atomic_store_n(&c->gen[slot % APP_CTX_CHUNK], gen, __ATOMIC_RELEASE); // the owner is visible before the generation
    t->apps++;
    view_unlock(t);
    return make_ctx(slot, gen);
}

bool app_ctx_remove(app_ctx_table_t *t, uint32_t ctx) {
    uint32_t slot = slot_of(ctx);
    if (slot == 0 || !app_ctx_valid(t, ctx)) {
        return false;
    }
    app_ctx_chunk_t *c = chunk_of(t, slot);
    c->last[slot % APP_CTX_CHUNK] = ctx >> APP_CTX_SLOT_BITS;
    view_lock(t);
    __atomic_store_n(&c->gen[slot % APP_CTX_CHUNK], 0u, __ATOMIC_RELEASE); // refused from here on
    __atomic_store_n(&c->owner[slot % APP_CTX_CHUNK], NULL, __ATOMIC_RELEASE);
    void *state = c->state[slot % APP_CTX_CHUNK];
    __atomic_store_n(&c->state[slot % APP_CTX_CHUNK], NULL, __ATOMIC_RELEASE);
    size_t i = 0;
    while (i < t->apps && order_get(t, i) != slot) {
        i++;
    }
    for (; i + 1 < t->apps; i++) {
        order_set(t, i, (uint8_t)order_get(t, i + 1));
    }
    t->apps--;
    view_unlock(t);
    if (state) {
        mem_release(t, state);
    }
    return true;
}

uint32_t app_ctx_add_app(app_ctx_table_t *t, void const *owner, uint32_t draw, app_ctx_names_fn names, char const **why) {
    char const **taken = NULL;
    size_t       n     = 0;
    if (owner && t->apps) {
        taken = mem_alloc(t, 2u * t->apps * sizeof(*taken));
        if (!taken) {
            *why = NULL; // out of memory, not an id refusal
            return APP_CTX_NONE;
        }
        for (size_t i = 0; i < t->apps; i++) {
            names(app_ctx_app_at(t, i), &taken[n], &taken[n + 1]);
            n += 2;
        }
    }
    char const *id = NULL, *alias = NULL;
    if (owner) {
        names(owner, &id, &alias);
    }
    *why = owner ? gate_app_names_refusal(id, alias, taken, n) : NULL;
    if (taken) {
        mem_release(t, taken);
    }
    return *why ? APP_CTX_NONE : app_ctx_add(t, owner, draw);
}

uint32_t app_ctx_os(app_ctx_table_t const *t) {
    return app_ctx_ready(t) ? make_ctx(0, t->os_gen) : APP_CTX_NONE;
}

size_t app_ctx_apps(app_ctx_table_t const *t) {
    return t->apps;
}

void const *app_ctx_app_at(app_ctx_table_t const *t, size_t index) {
    if (index >= t->apps) {
        return NULL;
    }
    uint32_t slot = order_get(t, index);
    return chunk_of(t, slot)->owner[slot % APP_CTX_CHUNK];
}

uint32_t app_ctx_at(app_ctx_table_t const *t, size_t index) {
    if (index >= t->apps) {
        return APP_CTX_NONE;
    }
    uint32_t slot = order_get(t, index);
    return make_ctx(slot, gen_at(t, slot));
}

uint32_t app_ctx_of(app_ctx_table_t const *t, void const *owner) {
    if (!owner) {
        return app_ctx_os(t);
    }
    for (size_t i = 0; i < t->apps; i++) {
        if (app_ctx_app_at(t, i) == owner) {
            return app_ctx_at(t, i);
        }
    }
    return APP_CTX_NONE;
}

bool app_ctx_valid(app_ctx_table_t const *t, uint32_t ctx) {
    uint32_t slot = slot_of(ctx);
    uint32_t gen  = ctx >> APP_CTX_SLOT_BITS;
    if (gen == 0) {
        return false;
    }
    return slot == 0 ? t->os_gen == gen : gen_at(t, slot) == gen;
}

void app_ctx_count_refusal(app_ctx_table_t *t, uint32_t ctx) {
    __atomic_fetch_add(&t->refused, 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&t->last_refused, ctx, __ATOMIC_RELAXED);
}

bool app_ctx_resolve(app_ctx_table_t *t, uint32_t ctx, void const **owner) {
    if (!app_ctx_valid(t, ctx)) {
        app_ctx_count_refusal(t, ctx);
        return false;
    }
    uint32_t    slot = slot_of(ctx);
    void const *got  = NULL;
    if (slot != 0) {
        // The owner, then the generation again: a remove clears the generation first, and an app's slot never holds a
        // NULL owner, so a half-removed slot is refused rather than taken for the OS
        app_ctx_chunk_t *c = chunk_of(t, slot);
        got                = __atomic_load_n(&c->owner[slot % APP_CTX_CHUNK], __ATOMIC_ACQUIRE);
        if (!got || gen_at(t, slot) != ctx >> APP_CTX_SLOT_BITS) {
            app_ctx_count_refusal(t, ctx);
            return false;
        }
    }
    if (owner) {
        *owner = got;
    }
    return true;
}

// Racy on purpose between tasks: the worst case is one extra line in the log, never a lost refusal count
bool app_ctx_log_due(app_ctx_table_t *t, uint32_t now_ms, uint32_t *skipped) {
    if (t->logged_any && now_ms - t->logged_ms < APP_CTX_LOG_GAP_MS) {
        __atomic_fetch_add(&t->unlogged, 1u, __ATOMIC_RELAXED);
        return false;
    }
    t->logged_any = true;
    t->logged_ms  = now_ms;
    uint32_t n    = __atomic_exchange_n(&t->unlogged, 0u, __ATOMIC_RELAXED);
    if (skipped) {
        *skipped = n;
    }
    return true;
}

uint32_t app_ctx_self(app_ctx_table_t const *t, app_ctx_tls_t const *tls) {
    uint32_t ctx = tls->in_isr() ? APP_CTX_NONE : tls->get();
    return ctx != APP_CTX_NONE ? ctx : app_ctx_os(t);
}

uint32_t app_ctx_enter(app_ctx_table_t *t, app_ctx_tls_t const *tls, uint32_t ctx, bool *refused) {
    if (refused) {
        *refused = false;
    }
    if (tls->in_isr()) {
        return APP_CTX_NONE;
    }
    uint32_t previous = tls->get();
    if (!app_ctx_resolve(t, ctx, NULL)) {
        if (refused) {
            *refused = true;
        }
        ctx = app_ctx_os(t);
    }
    tls->set(ctx);
    return previous;
}

void app_ctx_leave(app_ctx_tls_t const *tls, uint32_t previous) {
    if (!tls->in_isr()) {
        tls->set(previous);
    }
}

uint32_t app_ctx_for_task(app_ctx_table_t const *t, app_ctx_tls_t const *tls, void const *owner, bool *unregistered) {
    uint32_t ctx = owner ? app_ctx_of(t, owner) : app_ctx_self(t, tls);
    if (unregistered) {
        *unregistered = ctx == APP_CTX_NONE;
    }
    return ctx != APP_CTX_NONE ? ctx : app_ctx_os(t);
}

void *app_ctx_state(app_ctx_table_t const *t, uint32_t ctx) {
    uint32_t slot = slot_of(ctx);
    if (slot == 0 || !app_ctx_valid(t, ctx)) {
        return NULL;
    }
    app_ctx_chunk_t *c = chunk_of(t, slot);
    return c ? __atomic_load_n(&c->state[slot % APP_CTX_CHUNK], __ATOMIC_ACQUIRE) : NULL;
}

void const *app_ctx_slot_owner(app_ctx_table_t const *t, uint32_t ctx) {
    uint32_t         slot = slot_of(ctx);
    app_ctx_chunk_t *c    = slot ? chunk_of(t, slot) : NULL;
    return c ? __atomic_load_n(&c->owner[slot % APP_CTX_CHUNK], __ATOMIC_ACQUIRE) : NULL;
}
