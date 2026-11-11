#pragma once
// Memory for apps (#181, #84 V12): where a buffer goes is the OS's rule, not the app's. Big things go to PSRAM
// (LICHEN_MEM_BULK); internal RAM (LICHEN_MEM_INTERNAL) is for the rare buffer that must be there, and each app may hold
// at most MEM_BOOK_INTERNAL_QUOTA of it. "fw mem" lists what every app holds.

#include "lichen_app.h"
#include "mem_book.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LICHEN_MEM_BULK     0x1u // PSRAM: anything big, and anything that is not read by DMA or in a tight loop
#define LICHEN_MEM_INTERNAL 0x2u // internal RAM, counted against the app's quota
#define LICHEN_MEM_ZERO     0x4u // zeroed, as calloc

// ctx is the owner's context (#230 E1b): an app passes the one lichen_register_app gave it, the OS code
// lichen_sys_ctx_os() (lichen_sys_app.h). The memory is counted under ctx. A stale or made-up ctx is refused (logged at
// most once per 10 s, counted in "fw app ctx"), never an abort.
//
// Exactly one of BULK and INTERNAL. NULL when the heap has no room, ctx is refused, or for INTERNAL when the app's
// quota would be passed (logged). The OS's ctx is counted, never refused for the quota. Any task, not an ISR.
void *lichen_alloc(lichen_ctx_t ctx, size_t size, uint32_t flags);
// As lichen_alloc, the block starting at a multiple of align (a power of two): an LVGL draw buffer, a cache-line
// bound benchmark buffer. Freed with lichen_free. Any task, not an ISR.
void *lichen_alloc_aligned(lichen_ctx_t ctx, size_t align, size_t size, uint32_t flags);
// Frees what lichen_alloc gave the same ctx (NULL is fine). Never free it with free() or heap_caps_free: the app's
// count would stay up. A refused ctx frees nothing (logged): a made-up ctx cannot free memory. Any task, not an ISR.
void lichen_free(lichen_ctx_t ctx, void *ptr);

// main.c, once: adds the per-app lines to "fw mem"
void lichen_mem_console_register(void);

#ifdef __cplusplus
}
#endif
