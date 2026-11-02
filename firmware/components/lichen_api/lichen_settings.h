#pragma once
// An app's settings (#84 T5): NVS namespace "a.<id>" from lichen_app_t.id (app_ns.h), so an app's keys never meet
// Lichen's or another app's, and a factory reset (#66) can wipe one app. Keys are at most 15 characters (NVS). A
// missing key or an unreadable NVS gives the default. Apps never open NVS themselves.

#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t app_settings_get_u8(lichen_app_t const *app, char const *key, uint8_t def);
bool    app_settings_set_u8(lichen_app_t const *app, char const *key, uint8_t value);
uint32_t app_settings_get_u32(lichen_app_t const *app, char const *key, uint32_t def);
bool     app_settings_set_u32(lichen_app_t const *app, char const *key, uint32_t value);
uint64_t app_settings_get_u64(lichen_app_t const *app, char const *key, uint64_t def);
bool     app_settings_set_u64(lichen_app_t const *app, char const *key, uint64_t value);
// Copies the value (or def) into out, always NUL-terminated
void app_settings_get_str(lichen_app_t const *app, char const *key, char *out, size_t cap, char const *def);
bool app_settings_set_str(lichen_app_t const *app, char const *key, char const *value);
// Removes a key; true when it is gone (also when it was never there)
bool app_settings_erase(lichen_app_t const *app, char const *key);
// Removes every key of the app (factory reset, #66)
bool app_settings_erase_all(lichen_app_t const *app);

// A key an earlier firmware kept in Lichen's own namespace (badge_fw), same name and type: copied into the app's once
// and read back (app_adopt_run). True when it was copied now. The old key stays for a rollback; a later call that
// finds it in both erases the old one (#84 T9). Call it before the first read, e.g. from start_background. A copy that
// fails leaves the old key and is tried again next boot (the app reads its default until then).
bool app_settings_adopt_u8(lichen_app_t const *app, char const *key);
bool app_settings_adopt_u32(lichen_app_t const *app, char const *key);

#ifdef __cplusplus
}
#endif
