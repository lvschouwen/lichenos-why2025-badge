#include "ptr_map.h"

#define PTR_MAP_MIN 16u

static uint32_t home(void const *p, uint32_t cap) {
    return (uint32_t)(((uintptr_t)p >> 3) * 2654435761u) & (cap - 1u);
}

// The index of p, or UINT32_MAX
static uint32_t find(ptr_map_t const *m, void const *p) {
    if (!m->cap) {
        return UINT32_MAX;
    }
    uint32_t i = home(p, m->cap);
    while (m->slot[i].p) {
        if (m->slot[i].p == p) {
            return i;
        }
        i = (i + 1u) & (m->cap - 1u);
    }
    return UINT32_MAX; // the load stays under 3/4, so an empty slot ends the probe
}

static void put(ptr_map_ent_t *slot, uint32_t cap, void *p, uint32_t owner) {
    uint32_t i = home(p, cap);
    while (slot[i].p) {
        i = (i + 1u) & (cap - 1u);
    }
    slot[i] = (ptr_map_ent_t){p, owner};
}

static bool grow(ptr_map_t *m, ptr_map_env_t const *env) {
    uint32_t       cap   = m->cap ? m->cap * 2u : PTR_MAP_MIN;
    ptr_map_ent_t *fresh = env->alloc_zeroed((size_t)cap * sizeof(*fresh));
    if (!fresh) {
        return false;
    }
    for (uint32_t i = 0; i < m->cap; i++) {
        if (m->slot[i].p) {
            put(fresh, cap, m->slot[i].p, m->slot[i].owner);
        }
    }
    if (m->slot) {
        env->release(m->slot);
    }
    m->slot = fresh;
    m->cap  = cap;
    return true;
}

bool ptr_map_note(ptr_map_t *m, ptr_map_env_t const *env, void *p, uint32_t owner) {
    if (!p || !owner) {
        return false;
    }
    uint32_t i = find(m, p);
    if (i != UINT32_MAX) {
        m->slot[i].owner = owner;
        return true;
    }
    if ((m->used + 1u) * 4u > m->cap * 3u && !grow(m, env)) {
        return false;
    }
    put(m->slot, m->cap, p, owner);
    m->used++;
    return true;
}

// Backward-shift deletion: no removed markers, so the table never needs a rebuild at its own size
static void erase(ptr_map_t *m, uint32_t i) {
    uint32_t mask = m->cap - 1u;
    for (uint32_t j = (i + 1u) & mask; m->slot[j].p; j = (j + 1u) & mask) {
        uint32_t k = home(m->slot[j].p, m->cap);
        bool     stays = i <= j ? (i < k && k <= j) : (i < k || k <= j); // its home lies after the hole: it cannot move up
        if (!stays) {
            m->slot[i] = m->slot[j];
            i          = j;
        }
    }
    m->slot[i] = (ptr_map_ent_t){0};
    m->used--;
}

bool ptr_map_forget(ptr_map_t *m, void *p) {
    uint32_t i = find(m, p);
    if (i == UINT32_MAX) {
        return false;
    }
    erase(m, i);
    return true;
}

bool ptr_map_take(ptr_map_t *m, uint32_t owner, uint32_t *cursor, void **p) {
    for (; *cursor < m->cap; (*cursor)++) {
        if (m->slot[*cursor].p && m->slot[*cursor].owner == owner) {
            *p = m->slot[*cursor].p;
            erase(m, *cursor); // an entry may shift into this index: the next call looks here first
            return true;
        }
    }
    *cursor = 0;
    return false;
}

void ptr_map_trim(ptr_map_t *m, ptr_map_env_t const *env) {
    if (m->slot && !m->used) {
        env->release(m->slot);
        *m = (ptr_map_t){0};
    }
}

size_t ptr_map_sweep(ptr_map_t *m, uint32_t owner, ptr_map_sweep_env_t const *env) {
    size_t   n      = 0;
    uint32_t cursor = 0;
    for (;;) {
        uint32_t start = cursor;
        void    *p;
        env->lock(env->user);
        bool got = ptr_map_take(m, owner, &cursor, &p);
        env->unlock(env->user);
        if (got) {
            env->free_block(env->user, p);
            n++;
        } else if (start == 0) {
            return n;
        }
    }
}
