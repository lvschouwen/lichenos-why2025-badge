#pragma once

#include <stdbool.h>
#include <stddef.h>

// badge.sh names files the BadgeVMS way, FLASH0:[DIR.SUB]name; the lean firmware (#61) keeps them on /storage.
// Pure logic, no ESP headers: also built into the host tests.

// FLASH0:[A.B]c -> /storage/A/B/c, FLASH0:c -> /storage/c. False for anything ota_path_allowed() refuses, any device
// but FLASH0, or a result that doesn't fit.
bool ota_path_to_posix(char const *vms, char *out, size_t cap);

// Where a push to vms is written before it is swapped in: OTA_TEMP_NAME in the same directory, as a /storage path
bool ota_temp_to_posix(char const *vms, char *out, size_t cap);

// Whether a /storage path (ota_path_to_posix's result) is one the on-air list's USB hold refuses to change besides the
// C6 folder (#454): the root's lora* and ota_* names (lora.toml, ota_token.txt) and MeshCore's directory, at any depth.
// Compared by segment, any case, trailing dots and spaces dropped; "." or ".." anywhere, or a path outside /storage,
// counts as held (fails closed).
bool ota_path_onair_held(char const *posix);
