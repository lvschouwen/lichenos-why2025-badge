#pragma once
// An app's hand-over of a blob an earlier firmware kept in Lichen's namespace (badge_fw), #123. MeshCore's identity
// (its private key) is the one such blob. Pure decision logic, host-tested in host_tests/legacy_blob_test.c;
// lichen_legacy.c is the NVS glue.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LB_READ_FOUND, LB_READ_MISSING, LB_READ_ERROR } lb_read_t; // ERROR: NVS could not answer

typedef struct {
    void *ctx;
    lb_read_t (*read)(void *ctx, uint8_t *buf, size_t cap, size_t *len); // the blob in badge_fw
    bool (*erase)(void *ctx);                                            // erased and committed
} lb_io_t;

typedef enum {
    LICHEN_BLOB_NONE,         // nothing to hand over (no blob, too short to be the app's, or unreadable): nothing erased
    LICHEN_BLOB_MOVED,        // the app stored it, then the old blob was erased
    LICHEN_BLOB_KEPT,         // the app could not store it: the old blob stays, tried again next boot
    LICHEN_BLOB_ERASE_FAILED, // the app stored it but the erase failed: the old blob stays (harmless, a later call
                              // finds the app already has the identity and does not call take)
} lichen_blob_t;

// The app's confirmation: true only when it has stored the blob durably
typedef bool (*lichen_blob_store_fn)(void *ctx, uint8_t const *blob, size_t len);

// Reads first and erases only after `store` said true. A read error, a missing blob or one shorter than min_len never
// erases anything. *len is the blob's size when it was read.
lichen_blob_t legacy_blob_run(lb_io_t const *io, uint8_t *buf, size_t cap, size_t min_len, lichen_blob_store_fn store,
                              void *store_ctx);

// Which blobs an app may take: only the one it owns ("meshcore" takes "identity")
bool legacy_blob_allowed(char const *app_id, char const *key);

#ifdef __cplusplus
}
#endif
