#pragma once
// Per-app settings rules (#84 T5). Pure, host-tested in host_tests/app_ns_test.c; lichen_settings.c is the NVS glue.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app's NVS namespace: "a." + its id, so it never meets Lichen's badge_fw or another app's. NVS namespaces are at
// most 15 characters, so an id is 1..13 of a-z 0-9 _. False (out untouched) for any other id or a cap under 16.
bool app_ns_name(char const *id, char *out, size_t cap);

// A key an earlier firmware kept in badge_fw (MeshCore's mc_name_set): copied into the app's namespace once, only when
// the app has none yet and the old one exists. The old key is erased only on a later boot that finds it already in the
// app's namespace (#84 T9), so a failed copy never loses it.
typedef enum { APP_ADOPT_NONE, APP_ADOPT_COPY, APP_ADOPT_DROP_LEGACY } app_adopt_t;
app_adopt_t app_adopt_plan(bool have_app, bool have_legacy);

// The whole adoption of one key (u8 or u32, carried as uint32_t) over a store: lichen_settings.c's NVS, a fake in the
// host test. Each call does one NVS step and says whether it worked.
typedef struct {
    void *ctx;
    bool (*get_app)(void *ctx, uint32_t *value);    // true when the app's namespace has the key
    bool (*get_legacy)(void *ctx, uint32_t *value); // true when badge_fw has it
    bool (*set_app)(void *ctx, uint32_t value);     // written and committed
    bool (*erase_app)(void *ctx);                   // a copy that did not read back: gone, so the next boot copies again
    bool (*erase_legacy)(void *ctx);
} app_adopt_io_t;

typedef enum {
    APP_ADOPTED_NOTHING,     // no old key (a fresh badge, or adopted and dropped earlier)
    APP_ADOPTED_COPIED,      // copied and read back equal; the old key stays until the next boot
    APP_ADOPTED_COPY_FAILED, // not written, or read back different (that copy erased): the old key stays, next boot tries again
    APP_ADOPTED_DROPPED,     // the app already had it: the old key is erased (its value is ignored)
    APP_ADOPTED_DROP_FAILED, // the same, but the erase failed: tried again next boot
} app_adopted_t;

// *legacy gets the old key's value when there was one
app_adopted_t app_adopt_run(app_adopt_io_t const *io, uint32_t *legacy);

#ifdef __cplusplus
}
#endif
