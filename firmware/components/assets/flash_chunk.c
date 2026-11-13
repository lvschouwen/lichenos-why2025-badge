#include "flash_chunk.h"

#include <string.h>

int flash_chunk_copy(flash_chunk_read_fn read, void *ctx, size_t off, uint8_t *dst, size_t len, uint8_t *bounce,
                     size_t cap) {
    if (!bounce || cap == 0) {
        return -1;
    }
    for (size_t at = 0; at < len; at += cap) {
        size_t n = len - at < cap ? len - at : cap;
        if (read(ctx, off + at, bounce, n) != 0) {
            return -1;
        }
        memcpy(dst + at, bounce, n);
    }
    return 0;
}

int flash_chunk_same(flash_chunk_read_fn read, void *ctx, size_t off, uint8_t const *expect, size_t len,
                     uint8_t *bounce, size_t cap) {
    if (!bounce || cap == 0) {
        return -1;
    }
    for (size_t at = 0; at < len; at += cap) {
        size_t n = len - at < cap ? len - at : cap;
        if (read(ctx, off + at, bounce, n) != 0) {
            return -1;
        }
        if (memcmp(bounce, expect + at, n) != 0) {
            return 0;
        }
    }
    return 1;
}
