#include "ota_bmp.h"

#define BMP_MAX_SIDE 16384

size_t ota_bmp565_row_bytes(uint32_t width) {
    return ((size_t)width * 2 + 3) & ~(size_t)3;
}

size_t ota_bmp565_file_size(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0 || width > BMP_MAX_SIDE || height > BMP_MAX_SIDE) {
        return 0;
    }
    return OTA_BMP_HEADER_LEN + ota_bmp565_row_bytes(width) * height;
}

static void put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

bool ota_bmp565_header(uint32_t width, uint32_t height, uint8_t *header) {
    size_t file = ota_bmp565_file_size(width, height);
    if (!file) {
        return false;
    }
    for (size_t i = 0; i < OTA_BMP_HEADER_LEN; i++) {
        header[i] = 0;
    }
    header[0] = 'B';
    header[1] = 'M';
    put32(header + 2, (uint32_t)file);
    put32(header + 10, OTA_BMP_HEADER_LEN); // where the pixels start
    put32(header + 14, 40);                 // BITMAPINFOHEADER
    put32(header + 18, width);
    put32(header + 22, (uint32_t)-(int32_t)height); // negative: the first row is the top one
    put16(header + 26, 1);                          // planes
    put16(header + 28, 16);                         // bits per pixel
    put32(header + 30, 3);                          // BI_BITFIELDS
    put32(header + 34, (uint32_t)(file - OTA_BMP_HEADER_LEN));
    put32(header + 54, 0xF800); // red, green, blue masks
    put32(header + 58, 0x07E0);
    put32(header + 62, 0x001F);
    return true;
}
