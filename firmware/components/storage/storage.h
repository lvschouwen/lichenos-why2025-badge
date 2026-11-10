#pragma once

#include <stdbool.h>
#include <stdint.h>

// The LittleFS drive on the "storage" partition (#300), mounted at STORAGE_BASE. LittleFS is copy-on-write: a write
// cut off by a power loss leaves the old file and no lost space, which FAT did not (#300). storage.bin (the build)
// and tools/pack_storage.py write the same on-disk config the badge uses (docs/architecture.md, "Flash drive").
#define STORAGE_BASE  "/storage"
#define STORAGE_LABEL "storage"
// Its LittleFS block (the flash erase sector); free-space checks round file sizes up to it (#299)
#define STORAGE_CLUSTER 4096

// Mounts the drive once at boot; false when it does not mount. A drive that does not mount is left as it is, never
// formatted here (#110): formatting it is an explicit user action.
bool storage_mount(void);
// The boot mount worked
bool storage_mounted(void);
// Free and total bytes on the drive; false when it is not mounted. Free is counted by walking every file's blocks
// (lfs_fs_size): cheap on a drive this size, but not for a tight loop.
bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes);
// "fw storage [path]" (#299): free space and the size of each entry under path; registered by storage_mount
void storage_console_register(void);

// The factory reset's own access before the boot mount (lichen_boot, #66), with the boot mount's config.
// storage_attach mounts the drive at STORAGE_BASE without formatting (false when it does not mount); storage_detach
// unmounts it again; storage_format makes an empty drive over the whole partition (unmounted ones only: detach first).
bool storage_attach(void);
void storage_detach(void);
bool storage_format(void);
