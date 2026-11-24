#pragma once
// The SD apps' live malloc blocks (#536): block pointer -> owner (an app's context value: slot and generation, never a
// record pointer), so the loader can free what an app left when it unloads. One map for every app: a free removes by the
// pointer alone, whichever context it runs under. Open addressing over a table the caller's allocator makes (PSRAM on the
// badge), grow-only; pointers are never dereferenced, so a made-up or twice-freed pointer does no harm here.
// Pure (host_tests/ptr_map_test.c). Not thread safe: the caller's lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void    *p; // NULL: empty slot
    uint32_t owner;
} ptr_map_ent_t;

typedef struct {
    ptr_map_ent_t *slot;
    uint32_t       cap;  // 0 (no table yet) or a power of two
    uint32_t       used;
} ptr_map_t;

typedef struct {
    void *(*alloc_zeroed)(size_t bytes); // NULL on failure
    void (*release)(void *table);
} ptr_map_env_t;

// p belongs to owner from now on (a pointer already in the map changes owner: the heap handed an address out again). False: p
// is NULL, owner is 0, or no memory for a bigger table; the map is as before.
bool ptr_map_note(ptr_map_t *m, ptr_map_env_t const *env, void *p, uint32_t owner);
// Takes p out whoever owns it. False when it is not in the map.
bool ptr_map_forget(ptr_map_t *m, void *p);
// Takes one pointer of owner out of the map, looking from *cursor on; *cursor is where to look next. False when none is left
// from there (then *cursor is 0). Entries move when others come and go, so a sweep repeats until a call that started at 0 finds none.
bool ptr_map_take(ptr_map_t *m, uint32_t owner, uint32_t *cursor, void **p);
// Gives the table back when no pointer is left.
void ptr_map_trim(ptr_map_t *m, ptr_map_env_t const *env);
