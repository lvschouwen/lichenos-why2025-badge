#pragma once

#include <stdbool.h>
#include <stddef.h>

// badge.sh names files the BadgeVMS way, FLASH0:[DIR.SUB]name; the lean firmware (#61) keeps them on /storage.
// Pure logic, no ESP headers: also built into the host tests.

// FLASH0:[A.B]c -> /storage/A/B/c, FLASH0:c -> /storage/c; SD0: the same under /sd (#528). False for anything
// ota_path_allowed() refuses, any device but those two, or a result that doesn't fit.
bool ota_path_to_posix(char const *vms, char *out, size_t cap);

// Whether vms names the SD card drive (SD0:), whose files need the card mounted
bool ota_path_is_sd(char const *vms);

// Where a push to vms is written before it is swapped in: OTA_TEMP_NAME in the same directory (so the same drive)
bool ota_temp_to_posix(char const *vms, char *out, size_t cap);

// Whether a /storage path (ota_path_to_posix's result) is one the on-air list's USB hold refuses to change besides the
// C6 folder (#454): the root's lora* and ota_* names (lora.toml, ota_token.txt) and MeshCore's directory, at any depth.
// Compared by segment, any case, trailing dots and spaces dropped; "." or ".." anywhere, or a path outside /storage,
// counts as held (fails closed).
bool ota_path_onair_held(char const *posix);
