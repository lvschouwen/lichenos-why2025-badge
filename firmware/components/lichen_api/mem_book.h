#pragma once
// The accounting behind lichen_alloc (#181): bytes each app holds in internal RAM and in PSRAM, and the per-app
// internal-RAM quota. A row is keyed by the owner's context (lichen_ctx_t, #230 E1b), which lichen_mem.c has checked;
// the label (the app id, or MEM_BOOK_OS) is only what "fw mem" prints. Pure, host-tested in
// host_tests/mem_book_test.c; lichen_mem.c is the heap glue and the lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEM_BOOK_OWNERS    25 // LICHEN_MAX_APPS apps and the OS; lichen_mem.c asserts it is enough
#define MEM_BOOK_OWNER_CAP 14 // an app id is at most 13 characters (app_ns.h)
#define MEM_BOOK_OS        "lichen" // the label of the OS's own allocations (label NULL): counted, never limited
// Internal RAM is what the display's DMA and the radios need (150 KB free is the floor, docs/architecture.md); one app
// may hold at most this much of it through lichen_alloc. Everything big goes to PSRAM (LICHEN_MEM_BULK).
#define MEM_BOOK_INTERNAL_QUOTA (64u * 1024u)

typedef struct {
    uint32_t ctx;                       // the owner's context; 0 (never a valid one) = free row
    char     owner[MEM_BOOK_OWNER_CAP]; // its label: the app id, MEM_BOOK_OS for the OS
    bool     os;                        // the OS's row: never refused for the quota
    uint32_t internal, internal_peak, bulk, bulk_peak;
    uint32_t blocks;  // allocations not freed yet
    uint32_t refused; // internal requests refused for the quota
} mem_use_t;

typedef struct {
    mem_use_t row[MEM_BOOK_OWNERS];
} mem_book_t;

typedef enum {
    MEM_CHARGE_OK,     // charged
    MEM_CHARGE_QUOTA,  // refused: an app's internal total would pass the quota (counted in refused)
    MEM_CHARGE_NO_ROW, // not charged: no row left for a new owner (or ctx 0). Bookkeeping never fails an allocation:
                       // the caller allocates anyway, unbooked
} mem_charge_t;

// Before an allocation of bytes for ctx. label names a new row (cut to MEM_BOOK_OWNER_CAP - 1; NULL: MEM_BOOK_OS); os
// marks the OS's row, which has no quota. Give a charge back with mem_book_release when the allocation fails.
mem_charge_t mem_book_charge(mem_book_t *b, uint32_t ctx, char const *label, bool os, bool internal, uint32_t bytes);
// After the allocation: the heap may hand out a little more than asked (alignment). Added without the quota check, so
// the free, which only knows the block's real size, gives back exactly what was charged.
void mem_book_adjust(mem_book_t *b, uint32_t ctx, bool internal, uint32_t extra);
typedef enum {
    MEM_RELEASE_OK,
    MEM_RELEASE_NO_ROW, // the owner has no row: allocated unbooked (table full), or freed under the wrong owner
    MEM_RELEASE_OVER,   // more than the owner holds: freed under the wrong owner, or allocated outside lichen_alloc.
                        // The count stops at zero.
} mem_release_t;

// A free, or the charge of an allocation that failed: one block fewer. Never goes below zero.
mem_release_t mem_book_release(mem_book_t *b, uint32_t ctx, bool internal, uint32_t bytes);
mem_use_t const *mem_book_find(mem_book_t const *b, uint32_t ctx); // NULL: the owner never allocated

#ifdef __cplusplus
}
#endif
