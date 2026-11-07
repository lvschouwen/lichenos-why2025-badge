#pragma once

// Heap inspection for the shell and main (#228, #216 decision 1): free and largest-block figures for the About and
// Benchmark screens and the boot log, and the diagnostic walk of the UI check. System surface: tools/layers.conf forbids
// lichen_sys_* in L6. Allocation is lichen_alloc (lichen_mem.h), never this.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LICHEN_SYS_MEM_DEFAULT,  // what malloc() draws from
    LICHEN_SYS_MEM_INTERNAL, // internal RAM, byte-addressable
    LICHEN_SYS_MEM_DMA,      // internal RAM a DMA engine can reach
    LICHEN_SYS_MEM_PSRAM,
} lichen_sys_mem_kind_t;

size_t lichen_sys_mem_free(lichen_sys_mem_kind_t kind);
size_t lichen_sys_mem_largest(lichen_sys_mem_kind_t kind); // largest block that can still be allocated
size_t lichen_sys_mem_total(lichen_sys_mem_kind_t kind);   // 0 when the badge has none (no PSRAM)
// Free internal bytes now and the least there ever were since boot
void lichen_sys_mem_internal(size_t *free_now, size_t *free_min);

// A block of the walk: its address and size, whether it is in use
typedef bool (*lichen_sys_mem_walk_fn)(void const *ptr, size_t size, bool used, void *user);
// Calls fn for every block of the heap, until it returns false
void lichen_sys_mem_walk(lichen_sys_mem_kind_t kind, lichen_sys_mem_walk_fn fn, void *user);
// Heap integrity check (true = sound, details are logged); slow, a diagnostic
bool lichen_sys_mem_check(lichen_sys_mem_kind_t kind);

#ifdef __cplusplus
}
#endif
