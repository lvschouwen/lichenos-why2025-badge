#include "legacy_blob.h"

#include <string.h>

lichen_blob_t legacy_blob_run(lb_io_t const *io, uint8_t *buf, size_t cap, size_t min_len, lichen_blob_store_fn store,
                              void *store_ctx) {
    size_t len = 0;
    if (!io || !buf || !store || io->read(io->ctx, buf, cap, &len) != LB_READ_FOUND || len < min_len || len > cap) {
        return LICHEN_BLOB_NONE;
    }
    if (!store(store_ctx, buf, len)) {
        return LICHEN_BLOB_KEPT;
    }
    return io->erase(io->ctx) ? LICHEN_BLOB_MOVED : LICHEN_BLOB_ERASE_FAILED;
}

bool legacy_blob_allowed(char const *app_id, char const *key) {
    return app_id && key && strcmp(app_id, "meshcore") == 0 && strcmp(key, "identity") == 0;
}
