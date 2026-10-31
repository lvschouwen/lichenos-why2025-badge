#pragma once
// An app's files (#84 T6): one directory per app on the storage drive, "/storage/<id>", made on first use. Everything
// else on the drive is the OS's (the C6 image under /storage/c6, lora.toml, the OTA token): an app never names a path
// outside its own directory, and its file shim refuses ".." (fs_shim.cpp).

#include "lichen_app.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// "/storage/<id>", valid for as long as the badge runs; NULL (logged) when the id is unusable or the directory cannot
// be made (no drive)
char const *lichen_fs_app_dir(lichen_app_t const *app);

// The drive the app directories live on: false (zeros) when it is not mounted
bool lichen_fs_drive_bytes(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif
