#pragma once
// Hand-over of an old badge_fw blob to the app that owns it (#123). The OS reads it, the app stores it in its own
// files, and only when it says so is the old copy erased. Never erased on a read error.

#include "legacy_blob.h"
#include "lichen_app.h"

#ifdef __cplusplus
extern "C" {
#endif

// Reads badge_fw/<key> into buf (cap bytes) and calls store(); see legacy_blob_run. The app wipes buf afterwards.
// LICHEN_BLOB_NONE (logged) for a key the app does not own.
lichen_blob_t lichen_legacy_blob_take(lichen_app_t const *app, char const *key, uint8_t *buf, size_t cap, size_t min_len,
                                      lichen_blob_store_fn store, void *store_ctx);

#ifdef __cplusplus
}
#endif
