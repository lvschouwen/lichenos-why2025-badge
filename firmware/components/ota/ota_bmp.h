// The screenshot's file format (#30): a top-down 16-bit BMP (BITMAPINFOHEADER + BI_BITFIELDS, RGB565 masks), so the
// panel's pixels go out as they sit in memory, with no conversion on the badge. Pure and host-tested.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_BMP_HEADER_LEN 66 // 14 file header + 40 info header + 12 bit masks

// Bytes of one row in the file (rows are padded to 4 bytes)
size_t ota_bmp565_row_bytes(uint32_t width);

// Size of the whole file, 0 when the image is empty or too large for a BMP
size_t ota_bmp565_file_size(uint32_t width, uint32_t height);

// Fill header (OTA_BMP_HEADER_LEN bytes) for a width x height image. False when the image is empty or too large.
bool ota_bmp565_header(uint32_t width, uint32_t height, uint8_t *header);
