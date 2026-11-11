#pragma once
// An app's settings (#84 T5): NVS namespace "a.<id>" from lichen_app_t.id (app_ns.h), so an app's keys never meet
// Lichen's or another app's, and a factory reset (#66) can wipe one app. Keys are at most 15 characters (NVS). A
// missing key or an unreadable NVS gives the default. Apps never open NVS themselves.
//
// ctx is the caller's own context (#230 E1c): an app passes the one lichen_register_app gave it. The namespace is the
// ctx's app's, so an app cannot reach another's keys. A stale or made-up ctx, the OS's (it has no namespace) or an app
// without a usable id is refused: reads give the default, writes false (a refused ctx is logged at most once per 10 s
// and counted in "fw app ctx", never an abort).

#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t app_settings_get_u8(lichen_ctx_t ctx, char const *key, uint8_t def);
bool    app_settings_set_u8(lichen_ctx_t ctx, char const *key, uint8_t value);
uint32_t app_settings_get_u32(lichen_ctx_t ctx, char const *key, uint32_t def);
bool     app_settings_set_u32(lichen_ctx_t ctx, char const *key, uint32_t value);
uint64_t app_settings_get_u64(lichen_ctx_t ctx, char const *key, uint64_t def);
bool     app_settings_set_u64(lichen_ctx_t ctx, char const *key, uint64_t value);
// Copies the value (or def) into out, always NUL-terminated
void app_settings_get_str(lichen_ctx_t ctx, char const *key, char *out, size_t cap, char const *def);
bool app_settings_set_str(lichen_ctx_t ctx, char const *key, char const *value);
// Removes a key; true when it is gone (also when it was never there)
bool app_settings_erase(lichen_ctx_t ctx, char const *key);
// Removes every key of the app (factory reset, #66)
bool app_settings_erase_all(lichen_ctx_t ctx);

#ifdef __cplusplus
}
#endif
