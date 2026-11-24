#pragma once

#include <stddef.h>
#include <stdint.h>

// "fw mem diff" (#547, developer mode): which internal-heap blocks appeared, went or changed size between two heap walks.
// The pure part: sorting and comparing two block lists. Host-tested in host_tests/mem_diff_test.c.

typedef struct {
    uint32_t addr;
    uint32_t size;
} mem_blk_t;

typedef enum { MEM_DIFF_ADDED, MEM_DIFF_REMOVED, MEM_DIFF_RESIZED } mem_diff_kind_t;

typedef struct {
    unsigned added_n, removed_n, resized_n;
    uint64_t added_bytes, removed_bytes;
    int64_t  net; // bytes held now minus bytes held then (resized blocks count by their change)
} mem_diff_sum_t;

// kind ADDED: now is set; REMOVED: was is set; RESIZED: both (same address, other size)
typedef void (*mem_diff_fn)(mem_diff_kind_t kind, mem_blk_t const *was, mem_blk_t const *now, void *user);

// Ascending by address: heap_caps_walk goes heap by heap, not in one address order
void mem_blk_sort(mem_blk_t *v, size_t n);

// Both lists sorted (mem_blk_sort). Calls fn (may be NULL) for each difference in address order; sum is always filled.
void mem_diff(mem_blk_t const *was, size_t was_n, mem_blk_t const *now, size_t now_n, mem_diff_sum_t *sum, mem_diff_fn fn,
              void *user);
