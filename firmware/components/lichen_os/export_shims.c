// What an SD app's malloc, calloc, realloc and free resolve to (#519): the OS's memory book (lichen_mem.h) under the
// calling app's context, bulk memory in PSRAM. The shims exist so a ported C program (#516's DOOM) needs no change; the
// names are mapped in tools/export_allow.txt. A block must be freed by the shim of the same app: lichen_free refuses a
// foreign context's pointer.
// The loader keeps the live blocks (export_blocks.h) and frees what an app left at unload (#536).
// Known limits: realloc of a block another app (or the OS) allocated copies it into a block of the caller's and then
// frees the old one under the caller's context, which lichen_free refuses (logged), so the old block leaks; an app
// reallocs only what it allocated. realloc's old size is heap_caps_get_allocated_size, the heap's rounded-up size, not
// what was asked for: the copy is capped by the new size and the old block's real extent, so it never reads past the
// block, but it can copy a few bytes of the rounding slack.
#include "lichen_app.h"
#include "export_blocks.h"
#include "lichen_mem.h"

#include "esp_heap_caps.h"

#include <string.h>

void *lichen_exp_malloc(size_t size) {
    lichen_ctx_t ctx = lichen_ctx_self();
    void        *p   = lichen_alloc(ctx, size, LICHEN_MEM_BULK);
    if (p) {
        sd_app_block_note(ctx, p);
    }
    return p;
}

void *lichen_exp_calloc(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) {
        return NULL;
    }
    lichen_ctx_t ctx = lichen_ctx_self();
    void        *p   = lichen_alloc(ctx, n * size, LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (p) {
        sd_app_block_note(ctx, p);
    }
    return p;
}

void lichen_exp_free(void *ptr) {
    lichen_ctx_t ctx = lichen_ctx_self();
    if (ptr) {
        sd_app_block_forget(ctx, ptr); // before the heap can hand the address out again
    }
    lichen_free(ctx, ptr);
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

// strdup booked under the calling app like malloc (#533): a block the app frees with free()
char *lichen_exp_strdup(char const *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char  *p = lichen_exp_malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}
