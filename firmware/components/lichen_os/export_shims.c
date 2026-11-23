// What an SD app's malloc, calloc, realloc and free resolve to (#519): the OS's memory book (lichen_mem.h) under the
// calling app's context, bulk memory in PSRAM. The shims exist so a ported C program (#516's DOOM) needs no change; the
// names are mapped in tools/export_allow.txt. A block must be freed by the shim of the same app: lichen_free refuses a
// foreign context's pointer.
// Known limits: realloc of a block another app (or the OS) allocated copies it into a block of the caller's and then
// frees the old one under the caller's context, which lichen_free refuses (logged), so the old block leaks; an app
// reallocs only what it allocated. realloc's old size is heap_caps_get_allocated_size, the heap's rounded-up size, not
// what was asked for: the copy is capped by the new size and the old block's real extent, so it never reads past the
// block, but it can copy a few bytes of the rounding slack.
#include "lichen_app.h"
#include "lichen_mem.h"

#include "esp_heap_caps.h"

#include <string.h>

void *lichen_exp_malloc(size_t size) {
    return lichen_alloc(lichen_ctx_self(), size, LICHEN_MEM_BULK);
}

void *lichen_exp_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    return lichen_alloc(lichen_ctx_self(), n * size, LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
}

void lichen_exp_free(void *ptr) {
    lichen_free(lichen_ctx_self(), ptr);
}

void *lichen_exp_realloc(void *ptr, size_t size) {
    if (!ptr) {
        return lichen_exp_malloc(size);
    }
    if (size == 0) {
        lichen_exp_free(ptr);
        return NULL;
    }
    void *fresh = lichen_exp_malloc(size);
    if (!fresh) {
        return NULL; // the old block stays valid, as realloc says
    }
    size_t old = heap_caps_get_allocated_size(ptr);
    memcpy(fresh, ptr, old < size ? old : size);
    lichen_exp_free(ptr);
    return fresh;
}
