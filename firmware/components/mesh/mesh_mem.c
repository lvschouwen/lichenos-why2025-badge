#include "mesh_mem.h"

#include "lichen_mem.h"
#include "meshcore_run.h"

#include <stdint.h>

void *mesh_mem_ring_alloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) {
        return NULL;
    }
    return lichen_alloc(meshcore_ctx(), count * size, LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
}

void *mesh_mem_file_alloc(size_t bytes) {
    void *p = lichen_alloc(meshcore_ctx(), bytes, LICHEN_MEM_BULK);
    return p ? p : lichen_alloc(meshcore_ctx(), bytes, LICHEN_MEM_INTERNAL);
}

void mesh_mem_free(void *ptr) {
    lichen_free(meshcore_ctx(), ptr);
}
