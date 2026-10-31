#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C6_MD5_HEX_LEN 32

// True for a non-empty name made of [A-Za-z0-9_], so it can't escape its APPS:[...] directory
bool c6_valid_firmware_name(char const *name);

// True if md5 starts with 32 hex digits; truncates anything after them (newline, md5sum filename)
bool c6_valid_md5(char *md5);

// Find the size of the app partition at app_offset in an ESP partition table image.
// Returns false if there is no app partition at that offset.
bool c6_app_partition_size(uint8_t const *table, size_t table_len, uint32_t app_offset, uint32_t *size);

// Find the otadata partition (data/ota) in an ESP partition table image. Its offset differs between layouts
// (0xd000 for esp_hosted, 0xe000 for Arduino's default_8MB.csv), so it has to come from the table being flashed.
// Returns false if the table has no otadata partition.
bool c6_otadata_region(uint8_t const *table, size_t table_len, uint32_t *offset, uint32_t *size);
