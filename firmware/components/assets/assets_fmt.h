// The assets image (#22): read-only files (fonts, later images) in the "assets" partition, read at boot.
// Built by tools/pack_assets.py from firmware/assets/. Pure logic, host-tested (host_tests/assets_fmt_test.c).
//
// Layout, all integers little-endian:
//   header  32 bytes  magic "LCHA", u16 format, u16 0, u32 count, u32 total_len, u32 crc32, 12 bytes 0
//   table   count x 48 bytes: name (40 bytes, NUL-padded, at most 39 characters), u32 offset, u32 size
//   data    each file at its offset (from the start of the image), 16-byte aligned
// crc32 (zlib's) covers bytes 32..total_len, so the table and every file. A new, incompatible layout gets a new
// format number: firmware only accepts the format it knows and falls back to its built-in fonts otherwise.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ASSETS_FORMAT     1
#define ASSETS_HEADER_LEN 32
#define ASSETS_ENTRY_LEN  48
#define ASSETS_NAME_MAX   40 // bytes in the table, including the terminating NUL

typedef enum {
    ASSETS_OK = 0,
    ASSETS_ERR_EMPTY,  // erased flash or no image
    ASSETS_ERR_MAGIC,  // something else is there
    ASSETS_ERR_FORMAT, // an image for a newer (or older) firmware
    ASSETS_ERR_SIZE,   // header claims more than the partition holds
    ASSETS_ERR_TABLE,  // a table entry is malformed or points outside the image
    ASSETS_ERR_CRC,    // damaged, e.g. an interrupted update
} assets_err_t;

typedef struct {
    uint8_t const *base;
    uint32_t       total_len;
    uint32_t       count;
} assets_view_t;

// Check an image of up to `avail` bytes (the partition size) and fill `view`. With check_crc false only the header
// and table are checked (to look at an image before it is complete).
assets_err_t assets_open(uint8_t const *data, size_t avail, bool check_crc, assets_view_t *view);

// Find a file by name; false when it is not there
bool assets_find(assets_view_t const *view, char const *name, uint8_t const **data, uint32_t *size);

// Name of entry i (0..count-1), or NULL
char const *assets_name(assets_view_t const *view, uint32_t i);

// zlib-compatible CRC-32; start with crc = 0
uint32_t assets_crc32(uint32_t crc, uint8_t const *data, size_t len);

char const *assets_err_str(assets_err_t err);
