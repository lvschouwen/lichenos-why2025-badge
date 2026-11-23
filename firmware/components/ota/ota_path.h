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
// Where a file streamed to the card in pieces is written first (#529): OTA_STREAM_TEMP_NAME in the same directory
bool ota_stream_temp_to_posix(char const *vms, char *out, size_t cap);

// Whether a /storage path (ota_path_to_posix's result) is one the on-air list's USB hold refuses to change besides the
// C6 folder (#454): the root's lora* and ota_* names (lora.toml, ota_token.txt) and MeshCore's directory, at any depth.
// Compared by segment, any case, trailing dots and spaces dropped; "." or ".." anywhere, or a path outside /storage,
// counts as held (fails closed).
bool ota_path_onair_held(char const *posix);

// The decisions of an upload that don't need hardware, so the host tests pin them (#528)
// Offset of the next '/' that ends a directory of a /storage or /sd path, after from (0: start), skipping the mount
// point itself (mkdir on it fails); 0 when no directory is left. Walk: for (n = next_dir(p, 0); n; n = next_dir(p, n)).
size_t ota_path_next_dir(char const *posix, size_t from);
// A rename that fails with err: FAT refuses to replace an existing file (EEXIST), so on SD0: the old file is removed
// and the rename tried again; LittleFS replaces in the one rename
bool ota_rename_retry(bool sd, int err);
// The card is needed and not mounted: the upload is a 409
bool ota_sd_missing(char const *vms, bool mounted);
// Allocation unit the room check rounds a file up to on the drive vms names
size_t ota_cluster_bytes(char const *vms);
// How an upload ended (place_file). OTA_PLACE_LOST: on SD0: the old file was removed and the second rename failed, so
// the new data exists only as the temp file and must stay there
typedef enum { OTA_PLACE_OK, OTA_PLACE_WRITE, OTA_PLACE_SWAP, OTA_PLACE_DIR, OTA_PLACE_LOST } ota_place_t;
// Whether the temp file is removed after an upload ended this way
bool ota_place_drops_temp(ota_place_t step);
