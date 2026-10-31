#include "assets_fmt.h"

#include <string.h>

static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t assets_crc32(uint32_t crc, uint8_t const *data, size_t len) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

assets_err_t assets_open(uint8_t const *data, size_t avail, bool check_crc, assets_view_t *view) {
    if (avail < ASSETS_HEADER_LEN || get32(data) == 0xFFFFFFFFu) {
        return ASSETS_ERR_EMPTY;
    }
    if (memcmp(data, "LCHA", 4) != 0) {
        return ASSETS_ERR_MAGIC;
    }
    if ((data[4] | data[5] << 8) != ASSETS_FORMAT) {
        return ASSETS_ERR_FORMAT;
    }
    uint32_t count = get32(data + 8);
    uint32_t total = get32(data + 12);
    if (total < ASSETS_HEADER_LEN || total > avail) {
        return ASSETS_ERR_SIZE;
    }
    if (count > (total - ASSETS_HEADER_LEN) / ASSETS_ENTRY_LEN) {
        return ASSETS_ERR_TABLE;
    }
    uint32_t data_start = ASSETS_HEADER_LEN + count * ASSETS_ENTRY_LEN;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t const *e    = data + ASSETS_HEADER_LEN + i * ASSETS_ENTRY_LEN;
        uint32_t       off  = get32(e + ASSETS_NAME_MAX);
        uint32_t       size = get32(e + ASSETS_NAME_MAX + 4);
        if (e[0] == '\0' || memchr(e, '\0', ASSETS_NAME_MAX) == NULL) {
            return ASSETS_ERR_TABLE;
        }
        if (off < data_start || off > total || size > total - off) {
            return ASSETS_ERR_TABLE;
        }
    }
    if (check_crc && assets_crc32(0, data + ASSETS_HEADER_LEN, total - ASSETS_HEADER_LEN) != get32(data + 16)) {
        return ASSETS_ERR_CRC;
    }
    view->base      = data;
    view->total_len = total;
    view->count     = count;
    return ASSETS_OK;
}

char const *assets_name(assets_view_t const *view, uint32_t i) {
    return i < view->count ? (char const *)view->base + ASSETS_HEADER_LEN + i * ASSETS_ENTRY_LEN : NULL;
}

bool assets_find(assets_view_t const *view, char const *name, uint8_t const **data, uint32_t *size) {
    for (uint32_t i = 0; i < view->count; i++) {
        uint8_t const *e = view->base + ASSETS_HEADER_LEN + i * ASSETS_ENTRY_LEN;
        if (strncmp((char const *)e, name, ASSETS_NAME_MAX) == 0) {
            *data = view->base + get32(e + ASSETS_NAME_MAX);
            *size = get32(e + ASSETS_NAME_MAX + 4);
            return true;
        }
    }
    return false;
}

char const *assets_err_str(assets_err_t err) {
    switch (err) {
        case ASSETS_OK: return "ok";
        case ASSETS_ERR_EMPTY: return "no assets image";
        case ASSETS_ERR_MAGIC: return "not an assets image";
        case ASSETS_ERR_FORMAT: return "assets image of another format";
        case ASSETS_ERR_SIZE: return "assets image larger than its partition";
        case ASSETS_ERR_TABLE: return "assets table damaged";
        case ASSETS_ERR_CRC: return "assets image damaged (CRC)";
    }
    return "?";
}
