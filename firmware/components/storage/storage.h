#pragma once

#include <stdbool.h>
#include <stdint.h>

// The FAT drive on the "storage" partition (wear-levelled), mounted at STORAGE_BASE
#define STORAGE_BASE "/storage"
// Its FAT allocation unit; free-space checks round file sizes up to it (#299)
#define STORAGE_CLUSTER 4096

// Mounts the drive once at boot; false when it does not mount. A drive that does not mount is left as it is, never
// formatted here (#110): formatting it is an explicit user action.
bool storage_mount(void);
// The boot mount worked
bool storage_mounted(void);
// Free and total bytes on the drive; false when it is not mounted
bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes);
// "fw storage [path]" (#299): free space and the size of each entry under path; registered by storage_mount
void storage_console_register(void);
