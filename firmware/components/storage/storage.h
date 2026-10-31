#pragma once

#include <stdbool.h>
#include <stdint.h>

// The FAT drive on the "storage" partition (wear-levelled), mounted at STORAGE_BASE
#define STORAGE_BASE "/storage"

bool storage_mount(void);
// Free and total bytes on the drive; false when it is not mounted
bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes);
