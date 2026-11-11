#include "app_ctx.h"

#include <string.h>

static uint32_t gen_of(uint32_t draw) {
    uint32_t gen = draw & APP_CTX_GEN_MASK;
    return gen ? gen : 1u; // 0 marks a free slot
}

static uint32_t make_ctx(uint32_t slot, uint32_t gen) {
    return (gen << APP_CTX_SLOT_BITS) | slot;
}

void app_ctx_init(app_ctx_table_t *t, uint32_t draw) {
    memset(t, 0, sizeof(*t));
    t->gen[0] = gen_of(draw);
}

bool app_ctx_ready(app_ctx_table_t const *t) {
    return t->gen[0] != 0;
}

uint32_t app_ctx_add(app_ctx_table_t *t, void const *owner, uint32_t draw) {
    uint32_t slot = (uint32_t)t->apps + 1u;
    if (!owner || !app_ctx_ready(t) || slot >= APP_CTX_SLOTS) {
        return APP_CTX_NONE;
    }
    t->owner[slot] = owner;
    t->gen[slot]   = gen_of(draw);
    t->apps++;
    return make_ctx(slot, t->gen[slot]);
}

uint32_t app_ctx_os(app_ctx_table_t const *t) {
    return app_ctx_ready(t) ? make_ctx(0, t->gen[0]) : APP_CTX_NONE;
}

size_t app_ctx_apps(app_ctx_table_t const *t) {
    return t->apps;
}

void const *app_ctx_app_at(app_ctx_table_t const *t, size_t index) {
    return index < t->apps ? t->owner[index + 1] : NULL;
}

uint32_t app_ctx_at(app_ctx_table_t const *t, size_t index) {
    return index < t->apps ? make_ctx((uint32_t)index + 1u, t->gen[index + 1]) : APP_CTX_NONE;
}

uint32_t app_ctx_of(app_ctx_table_t const *t, void const *owner) {
    if (!owner) {
        return app_ctx_os(t);
    }
    for (size_t i = 0; i < t->apps; i++) {
        if (t->owner[i + 1] == owner) {
            return app_ctx_at(t, i);
        }
    }
    return APP_CTX_NONE;
}

bool app_ctx_valid(app_ctx_table_t const *t, uint32_t ctx) {
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    uint32_t gen  = ctx >> APP_CTX_SLOT_BITS;
    if (slot >= APP_CTX_SLOTS || gen == 0 || (slot > t->apps)) {
        return false;
    }
    return t->gen[slot] == gen;
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
    if (owner) {
        *owner = t->owner[ctx & ((1u << APP_CTX_SLOT_BITS) - 1u)];
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
