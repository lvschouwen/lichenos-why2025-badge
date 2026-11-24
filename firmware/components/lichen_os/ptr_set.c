#include "ptr_set.h"

#define PTR_SET_MIN 16

static uint32_t home(void const *p, uint32_t cap) {
    return (uint32_t)(((uintptr_t)p >> 3) * 2654435761u) & (cap - 1u);
}

#define NOT_FOUND UINT32_MAX

// The index of p, or NOT_FOUND
static uint32_t find(ptr_set_t const *s, void const *p) {
    if (!s->cap) {
        return NOT_FOUND;
    }
    uint32_t i = home(p, s->cap);
    for (uint32_t n = 0; n < s->cap && s->slot[i]; n++, i = (i + 1) & (s->cap - 1u)) {
        if (s->slot[i] == p) {
            return i;
        }
    }
    return NOT_FOUND;
}

static void put(void **slot, uint32_t cap, void *p) {
    uint32_t i = home(p, cap);
    while (slot[i]) {
        i = (i + 1) & (cap - 1u);
    }
    slot[i] = p;
}

// A new table when the old one is three quarters full of pointers and markers: bigger when the pointers are what fill it,
// as big when it is mostly markers
static bool rehash(ptr_set_t *s, ptr_set_env_t const *env) {
    uint32_t cap = s->cap ? s->cap : PTR_SET_MIN;
    if ((s->used + 1u) * 2u > cap) {
        cap *= 2u;
    }
    void **fresh = env->alloc_zeroed((size_t)cap * sizeof(void *));
    if (!fresh) {
        return false;
    }
    for (uint32_t i = 0; i < s->cap; i++) {
        if (s->slot[i] && s->slot[i] != PTR_SET_GONE) {
            put(fresh, cap, s->slot[i]);
        }
    }
    if (s->slot) {
        env->release(s->slot);
    }
    s->slot = fresh;
    s->cap  = cap;
    s->gone = 0;
    s->scan = 0;
    return true;
}

bool ptr_set_add(ptr_set_t *s, ptr_set_env_t const *env, void *p) {
    if (!p || p == PTR_SET_GONE) {
        return false;
    }
    if (find(s, p) != NOT_FOUND) {
        return true;
    }
    if (!s->cap || (s->used + s->gone + 1u) * 4u > s->cap * 3u) {
        if (!rehash(s, env)) {
            return false;
        }
    }
    put(s->slot, s->cap, p);
    s->used++;
    s->scan = 0;
    return true;
}

bool ptr_set_remove(ptr_set_t *s, void *p) {
    uint32_t i = find(s, p);
    if (i == NOT_FOUND) {
        return false;
    }
    s->slot[i] = PTR_SET_GONE;
    s->used--;
    s->gone++;
    return true;
}

void *ptr_set_pop(ptr_set_t *s) {
    for (; s->scan < s->cap; s->scan++) {
        void *p = s->slot[s->scan];
        if (p && p != PTR_SET_GONE) {
            s->slot[s->scan] = PTR_SET_GONE;
            s->used--;
            s->gone++;
            return p;
        }
    }
    return NULL;
}

void ptr_set_release(ptr_set_t *s, ptr_set_env_t const *env) {
    if (s->slot) {
        env->release(s->slot);
    }
    *s = (ptr_set_t){0};
}
