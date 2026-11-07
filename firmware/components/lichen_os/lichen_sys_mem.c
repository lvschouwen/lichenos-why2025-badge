// lichen_sys_mem.h (#228): the shell's and main's heap figures over heap_caps, so that only lichen_os calls it.
#include "lichen_sys_mem.h"

#include "esp_heap_caps.h"

static uint32_t caps_of(lichen_sys_mem_kind_t kind) {
    switch (kind) {
    case LICHEN_SYS_MEM_INTERNAL:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    case LICHEN_SYS_MEM_DMA:
        return MALLOC_CAP_DMA;
    case LICHEN_SYS_MEM_PSRAM:
        return MALLOC_CAP_SPIRAM;
    default:
        return MALLOC_CAP_DEFAULT;
    }
}

size_t lichen_sys_mem_free(lichen_sys_mem_kind_t kind) {
    return heap_caps_get_free_size(caps_of(kind));
}

size_t lichen_sys_mem_largest(lichen_sys_mem_kind_t kind) {
    return heap_caps_get_largest_free_block(caps_of(kind));
}

size_t lichen_sys_mem_total(lichen_sys_mem_kind_t kind) {
    return heap_caps_get_total_size(caps_of(kind));
}

void lichen_sys_mem_internal(size_t *free_now, size_t *free_min) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, caps_of(LICHEN_SYS_MEM_INTERNAL));
    *free_now = info.total_free_bytes;
    *free_min = info.minimum_free_bytes;
}

typedef struct {
    lichen_sys_mem_walk_fn fn;
    void                  *user;
} walk_ctx_t;

static bool walk_block(walker_heap_into_t heap, walker_block_info_t block, void *user) {
    (void)heap;
    walk_ctx_t *ctx = user;
    return ctx->fn(block.ptr, block.size, block.used, ctx->user);
}

void lichen_sys_mem_walk(lichen_sys_mem_kind_t kind, lichen_sys_mem_walk_fn fn, void *user) {
    walk_ctx_t ctx = {.fn = fn, .user = user};
    heap_caps_walk(kind == LICHEN_SYS_MEM_INTERNAL ? MALLOC_CAP_INTERNAL : caps_of(kind), walk_block, &ctx);
}

bool lichen_sys_mem_check(lichen_sys_mem_kind_t kind) {
    return heap_caps_check_integrity(caps_of(kind), true);
}
