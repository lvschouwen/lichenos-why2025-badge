// See sd_bounce.h (#121)
#include "sd_bounce.h"

bool sd_bounce_needed(bool in_psram, bool dma_aligned) {
    return in_psram || !dma_aligned;
}

bool sd_bounce_start(sd_bounce_iter_t *it, uint32_t sector, uint32_t count, size_t sector_size, size_t bounce_bytes) {
    if (sector_size == 0 || bounce_bytes < sector_size) {
        return false;
    }
    size_t per = bounce_bytes / sector_size;
    *it        = (sd_bounce_iter_t){
               .sector      = sector,
               .left        = count,
               .per_chunk   = per > UINT32_MAX ? UINT32_MAX : (uint32_t)per,
               .offset      = 0,
               .sector_size = sector_size,
    };
    return true;
}

bool sd_bounce_next(sd_bounce_iter_t *it, sd_bounce_chunk_t *out) {
    if (it->left == 0) {
        return false;
    }
    uint32_t n  = it->left < it->per_chunk ? it->left : it->per_chunk;
    *out        = (sd_bounce_chunk_t){.sector = it->sector,
                                      .count  = n,
                                      .offset = it->offset,
                                      .bytes  = (size_t)n * it->sector_size};
    it->sector += n;
    it->left   -= n;
    it->offset += out->bytes;
    return true;
}
