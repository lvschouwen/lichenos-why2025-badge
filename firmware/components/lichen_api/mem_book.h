#pragma once
// The accounting behind lichen_alloc (#181): bytes each app holds in internal RAM and in PSRAM, and the per-app
// internal-RAM quota. Pure, host-tested in host_tests/mem_book_test.c; lichen_mem.c is the heap glue and the lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_BOOK_OWNERS    25 // LICHEN_MAX_APPS apps and the OS; lichen_mem.c asserts it is enough
#define MEM_BOOK_OWNER_CAP 14 // an app id is at most 13 characters (app_ns.h)
#define MEM_BOOK_OS        "lichen" // the OS's own allocations (app NULL): counted, never limited
// Internal RAM is what the display's DMA and the radios need (150 KB free is the floor, docs/architecture.md); one app
// may hold at most this much of it through lichen_alloc. Everything big goes to PSRAM (LICHEN_MEM_BULK).
#define MEM_BOOK_INTERNAL_QUOTA (64u * 1024u)

typedef struct {
    char     owner[MEM_BOOK_OWNER_CAP]; // "" = free row
    uint32_t internal, internal_peak, bulk, bulk_peak;
    uint32_t blocks;  // allocations not freed yet
    uint32_t refused; // internal requests refused for the quota
} mem_use_t;

typedef struct {
    mem_use_t row[MEM_BOOK_OWNERS];
} mem_book_t;

// Before an allocation of bytes: false when an app's internal total would pass the quota (counted in refused) or the
// table has no row for a new owner. Charged on true; give it back with mem_book_release when the allocation fails.
bool mem_book_charge(mem_book_t *b, char const *owner, bool internal, uint32_t bytes);
// After the allocation: the heap may hand out a little more than asked (alignment). Added without the quota check, so
// the free, which only knows the block's real size, gives back exactly what was charged.
void mem_book_adjust(mem_book_t *b, char const *owner, bool internal, uint32_t extra);
// A free, or the charge of an allocation that failed: one block fewer. Never goes below zero.
void mem_book_release(mem_book_t *b, char const *owner, bool internal, uint32_t bytes);
mem_use_t const *mem_book_find(mem_book_t const *b, char const *owner); // NULL: the owner never allocated

#ifdef __cplusplus
}
#endif
