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
// Removes every key of the app, its secrets included (factory reset #66, Clear data #233)
bool app_settings_erase_all(lichen_ctx_t ctx);

// The app's secrets (#288): passwords, tokens, keys. A key is one of the app's secrets when it starts with the prefix
// the app declared (lichen_app_t.secret_prefix, at most 14 characters in all). The plain calls above refuse such a key
// for every type (reads give the default, writes false); only these reach it. A secret is never in the SD backup, never
// kept over a factory reset, never printed by the OS; Clear data and a factory reset erase it. The app must never log
// one either, and should wipe its own copies after use. Values are at most APP_SECRET_VALUE_MAX (64) characters.
// Reads the secret into out (always terminated, emptied on failure): false when the key is not under the app's prefix,
// missing, too long for cap or NVS cannot be read. An empty value reads back as true.
bool app_secret_get(lichen_ctx_t ctx, char const *key, char *out, size_t cap);
// Saves a secret and commits; false when the key is not under the prefix, the value is too long or the write fails
bool app_secret_set(lichen_ctx_t ctx, char const *key, char const *value);
// Removes a secret; true when it is gone (also when it was never there)
bool app_secret_erase(lichen_ctx_t ctx, char const *key);

#ifdef __cplusplus
}
#endif
