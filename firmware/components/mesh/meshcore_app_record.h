#pragma once
// What MeshCore's app record tells the OS by name (#230 E1d): kept in a pure file so a host test pins it
// (host_tests/meshcore_app_record_test.c): a dropped keep key would erase the last advert at a factory reset (#52).
#include "settings_backup.h"

#include <stdbool.h>
#include <stddef.h>

// The app's id (lichen_app_t.id): its settings namespace a.meshcore and its directory /storage/meshcore
#define MESHCORE_APP_ID "meshcore"
#define MESHCORE_ALIAS  "mesh"

#define MESHCORE_STORAGE_ROOT   MESHCORE_APP_ID // /storage/meshcore: the identity and keys
#define MESHCORE_STORAGE_SECRET true
#define MESHCORE_BACKUP_KEYS    3

extern char const *const meshcore_keep_keys[]; // NULL-terminated
extern sb_key_t const    meshcore_backup_keys[MESHCORE_BACKUP_KEYS];
