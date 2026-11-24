#pragma once
// A set of live block pointers (#536): the SD apps' malloc blocks, so the loader can free what an app left when it unloads.
// Open addressing over a table the caller's allocator makes (PSRAM on the badge), pointers only: the OS never reads through
// an entry, so a pointer an app made up, freed twice or never allocated cannot do harm here. Pure (host_tests/ptr_set_test.c).
// Not thread safe: the caller's lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void   **slot; // NULL: empty, PTR_SET_GONE: removed (probing goes on past it)
    uint32_t cap;  // 0 (no table yet) or a power of two
    uint32_t used; // live pointers
    uint32_t gone; // removed markers in the table
    uint32_t scan; // where ptr_set_pop looks next
} ptr_set_t;

typedef struct {
    void *(*alloc_zeroed)(size_t bytes); // NULL on failure
    void (*release)(void *table);
} ptr_set_env_t;

#define PTR_SET_GONE ((void *)1) // never a block's address (blocks are aligned)

// Adds p (a pointer already in the set stays once). False: no memory for the table, p is not in the set (the set is as before).
bool ptr_set_add(ptr_set_t *s, ptr_set_env_t const *env, void *p);
// Removes p. False when it is not in the set (a pointer the app never got from malloc, or freed twice).
bool ptr_set_remove(ptr_set_t *s, void *p);
// Takes any one pointer out of the set; NULL when empty. Draining the set visits every pointer once.
void *ptr_set_pop(ptr_set_t *s);
// Drains the set: release(user, p) is called once for every pointer in it and answers the bytes it freed. Returns how many
// pointers there were, the bytes summed in *bytes. A pointer removed before (the app freed it itself) is not visited.
size_t ptr_set_drain(ptr_set_t *s, uint32_t (*release)(void *user, void *p), void *user, uint32_t *bytes);
// Gives the table back and empties the set (call once it is drained).
void ptr_set_release(ptr_set_t *s, ptr_set_env_t const *env);
