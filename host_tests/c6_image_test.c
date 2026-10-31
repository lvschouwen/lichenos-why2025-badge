// Host tests for the C6 firmware image checks done before the P4 reflashes the C6
#include "c6_image.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

static void put_entry(uint8_t *entry, uint8_t type, uint8_t subtype, uint32_t offset, uint32_t size) {
    memset(entry, 0, 32);
    entry[0] = 0xAA;
    entry[1] = 0x50;
    entry[2] = type;
    entry[3] = subtype;
    put_le32(&entry[4], offset);
    put_le32(&entry[8], size);
}

// Same layout as c6/partitions.esp32c6.csv
static size_t build_hosted_table(uint8_t *table, size_t len) {
    memset(table, 0xFF, len);
    put_entry(&table[0], 0x01, 0x02, 0x9000, 0x4000);     // nvs
    put_entry(&table[32], 0x01, 0x00, 0xd000, 0x2000);    // otadata
    put_entry(&table[64], 0x01, 0x01, 0xf000, 0x1000);    // phy_init
    put_entry(&table[96], 0x00, 0x10, 0x10000, 0x180000); // ota_0
    put_entry(&table[128], 0x00, 0x11, 0x190000, 0x180000); // ota_1
    table[160] = 0xEB;                                     // MD5 entry ends the table
    table[161] = 0xEB;
    return len;
}

static void test_firmware_names(void) {
    CHECK(c6_valid_firmware_name("why2025_firmware_ota_c6"));
    CHECK(c6_valid_firmware_name("meshcore_internal"));
    CHECK(!c6_valid_firmware_name(NULL));
    CHECK(!c6_valid_firmware_name(""));
    CHECK(!c6_valid_firmware_name("../etc"));
    CHECK(!c6_valid_firmware_name("a]b"));
    CHECK(!c6_valid_firmware_name("APPS:x"));
    CHECK(!c6_valid_firmware_name("with space"));
}

static void test_md5(void) {
    char plain[]   = "0123456789abcdef0123456789ABCDEF";
    char newline[] = "0123456789abcdef0123456789abcdef\n";
    char md5sum[]  = "0123456789abcdef0123456789abcdef  firmware.bin\n";
    char short_[]  = "0123456789abcdef";
    char nonhex[]  = "0123456789abcdef0123456789abcdeg";
    char empty[]   = "";

    CHECK(c6_valid_md5(plain));
    CHECK(c6_valid_md5(newline) && strlen(newline) == C6_MD5_HEX_LEN);
    CHECK(c6_valid_md5(md5sum) && strlen(md5sum) == C6_MD5_HEX_LEN);
    CHECK(!c6_valid_md5(short_));
    CHECK(!c6_valid_md5(nonhex));
    CHECK(!c6_valid_md5(empty));
    CHECK(!c6_valid_md5(NULL));
}

static void test_partition_table(void) {
    uint8_t  table[3072];
    uint32_t size = 0;
    size_t   len  = build_hosted_table(table, sizeof(table));

    CHECK(c6_app_partition_size(table, len, 0x10000, &size) && size == 0x180000);
    CHECK(c6_app_partition_size(table, len, 0x190000, &size) && size == 0x180000);
    // A data partition at the offset doesn't count
    CHECK(!c6_app_partition_size(table, len, 0x9000, &size));
    CHECK(!c6_app_partition_size(table, len, 0x20000, &size));
    // Truncated before the app entry
    CHECK(!c6_app_partition_size(table, 96, 0x10000, &size));
    // Partial trailing entry is ignored rather than read past the end
    CHECK(!c6_app_partition_size(table, 96 + 16, 0x10000, &size));
    // Blank flash
    memset(table, 0xFF, sizeof(table));
    CHECK(!c6_app_partition_size(table, sizeof(table), 0x10000, &size));
}

// Arduino default_8MB.csv: nvs 0x9000, otadata 0xe000, app0 0x10000
static void test_otadata(void) {
    uint8_t  table[3072];
    uint32_t offset = 0;
    uint32_t size   = 0;
    size_t   len    = build_hosted_table(table, sizeof(table));

    CHECK(c6_otadata_region(table, len, &offset, &size) && offset == 0xd000 && size == 0x2000);

    memset(table, 0xFF, sizeof(table));
    put_entry(&table[0], 0x01, 0x02, 0x9000, 0x5000);  // nvs
    put_entry(&table[32], 0x01, 0x00, 0xe000, 0x2000); // otadata
    put_entry(&table[64], 0x00, 0x10, 0x10000, 0x330000);
    CHECK(c6_otadata_region(table, sizeof(table), &offset, &size) && offset == 0xe000 && size == 0x2000);

    // nvs (data/nvs) is not otadata, and no otadata means nothing to erase
    memset(table, 0xFF, sizeof(table));
    put_entry(&table[0], 0x01, 0x02, 0x9000, 0x5000);
    put_entry(&table[32], 0x00, 0x00, 0x10000, 0x100000); // app/factory
    CHECK(!c6_otadata_region(table, sizeof(table), &offset, &size));
    // Entry past the end of a truncated table is not read
    build_hosted_table(table, sizeof(table));
    CHECK(!c6_otadata_region(table, 32 + 16, &offset, &size));
    memset(table, 0xFF, sizeof(table));
    CHECK(!c6_otadata_region(table, sizeof(table), &offset, &size));
}

int main(void) {
    test_firmware_names();
    test_md5();
    test_partition_table();
    test_otadata();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("c6_image_test: all checks passed\n");
    return 0;
}
