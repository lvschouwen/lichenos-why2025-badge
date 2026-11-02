#include "c6_image.h"

#include <ctype.h>
#include <string.h>

#define PART_ENTRY_SIZE 32
#define PART_MAGIC      0x50AA
#define PART_TYPE_APP   0x00
#define PART_TYPE_DATA  0x01
#define PART_SUBTYPE_OTA 0x00

#define IMAGE_MAGIC     0xE9
#define APP_DESC_OFFSET 0x20 // the image header (24 bytes) and the first segment's header (8)
#define APP_DESC_MAGIC  0xABCD5432
#define APP_DESC_SHA    0x90 // magic, secure_version, reserv1[2], version[32], project_name[32], time, date, idf_ver

static uint32_t read_le32(uint8_t const *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool c6_valid_firmware_name(char const *name) {
    if (!name || !*name) {
        return false;
    }
    for (char const *c = name; *c; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_') {
            return false;
        }
    }
    return true;
}

bool c6_valid_md5(char *md5) {
    if (!md5 || strlen(md5) < C6_MD5_HEX_LEN) {
        return false;
    }
    for (int i = 0; i < C6_MD5_HEX_LEN; i++) {
        if (!isxdigit((unsigned char)md5[i])) {
            return false;
        }
    }
    md5[C6_MD5_HEX_LEN] = '\0';
    return true;
}

bool c6_app_partition_size(uint8_t const *table, size_t table_len, uint32_t app_offset, uint32_t *size) {
    for (size_t pos = 0; pos + PART_ENTRY_SIZE <= table_len; pos += PART_ENTRY_SIZE) {
        uint8_t const *entry = &table[pos];
        if ((entry[0] | (entry[1] << 8)) != PART_MAGIC) {
            // End of the table (0xFF padding or the MD5 entry)
            break;
        }
        if (entry[2] == PART_TYPE_APP && read_le32(&entry[4]) == app_offset) {
            *size = read_le32(&entry[8]);
            return true;
        }
    }
    return false;
}

bool c6_otadata_region(uint8_t const *table, size_t table_len, uint32_t *offset, uint32_t *size) {
    for (size_t pos = 0; pos + PART_ENTRY_SIZE <= table_len; pos += PART_ENTRY_SIZE) {
        uint8_t const *entry = &table[pos];
        if ((entry[0] | (entry[1] << 8)) != PART_MAGIC) {
            break;
        }
        if (entry[2] == PART_TYPE_DATA && entry[3] == PART_SUBTYPE_OTA) {
            *offset = read_le32(&entry[4]);
            *size   = read_le32(&entry[8]);
            return true;
        }
    }
    return false;
}

bool c6_app_elf_sha256(uint8_t const *img, size_t len, uint8_t out[C6_APP_SHA_LEN]) {
    _Static_assert(APP_DESC_OFFSET + APP_DESC_SHA + C6_APP_SHA_LEN == C6_APP_SHA_NEEDED, "sha offset");
    if (!img || len < C6_APP_SHA_NEEDED || img[0] != IMAGE_MAGIC || read_le32(&img[APP_DESC_OFFSET]) != APP_DESC_MAGIC) {
        return false;
    }
    uint8_t const *sha = &img[APP_DESC_OFFSET + APP_DESC_SHA];
    uint8_t        any = 0;
    for (int i = 0; i < C6_APP_SHA_LEN; i++) {
        any |= sha[i];
    }
    if (!any) {
        return false;
    }
    memcpy(out, sha, C6_APP_SHA_LEN);
    return true;
}
