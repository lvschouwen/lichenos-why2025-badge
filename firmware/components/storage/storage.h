#pragma once

#include <stdbool.h>
#include <stdint.h>

// The FAT drive on the "storage" partition (wear-levelled), mounted at STORAGE_BASE
#define STORAGE_BASE "/storage"

// Mounts the drive once at boot; false when it does not mount. A drive that does not mount is left as it is, never
// formatted here (#110): formatting it is an explicit user action.
bool storage_mount(void);
// The boot mount worked
bool storage_mounted(void);
// Free and total bytes on the drive; false when it is not mounted
bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes);
