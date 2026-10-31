// Host tests for the assets image format (#22). With an argument, also checks an image built by
// tools/pack_assets.py from host_tests/vectors/assets/ (see CMakeLists.txt), so the packer and parser agree.
#include "assets_fmt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// A two-file image like the packer makes: "a.txt" = "hello", "fonts/b.bin" = 20 bytes 0..19
static size_t build(uint8_t *img, size_t cap) {
    memset(img, 0, cap);
    size_t table = ASSETS_HEADER_LEN;
    size_t off_a = table + 2 * ASSETS_ENTRY_LEN; // 128, already 16-aligned
    size_t off_b = off_a + 16;
    size_t total = off_b + 20;
    memcpy(img, "LCHA", 4);
    img[4] = ASSETS_FORMAT;
    put32(img + 8, 2);
    put32(img + 12, (uint32_t)total);
    strcpy((char *)img + table, "a.txt");
    put32(img + table + 40, (uint32_t)off_a);
    put32(img + table + 44, 5);
    strcpy((char *)img + table + ASSETS_ENTRY_LEN, "fonts/b.bin");
    put32(img + table + ASSETS_ENTRY_LEN + 40, (uint32_t)off_b);
    put32(img + table + ASSETS_ENTRY_LEN + 44, 20);
    memcpy(img + off_a, "hello", 5);
    for (int i = 0; i < 20; i++) {
        img[off_b + i] = (uint8_t)i;
    }
    put32(img + 16, assets_crc32(0, img + ASSETS_HEADER_LEN, total - ASSETS_HEADER_LEN));
    return total;
}

static void unit_tests(void) {
    uint8_t       img[512];
    assets_view_t v;
    size_t        total = build(img, sizeof(img));

    // zlib's check value for "123456789"
    CHECK(assets_crc32(0, (uint8_t const *)"123456789", 9) == 0xCBF43926u);
    CHECK(assets_crc32(assets_crc32(0, (uint8_t const *)"1234", 4), (uint8_t const *)"56789", 5) == 0xCBF43926u);

    CHECK(assets_open(img, sizeof(img), true, &v) == ASSETS_OK);
    CHECK(v.count == 2 && v.total_len == total);
    uint8_t const *data;
    uint32_t       size;
    CHECK(assets_find(&v, "a.txt", &data, &size) && size == 5 && memcmp(data, "hello", 5) == 0);
    CHECK(assets_find(&v, "fonts/b.bin", &data, &size) && size == 20 && data[19] == 19);
    CHECK(!assets_find(&v, "fonts/b", &data, &size));
    CHECK(!assets_find(&v, "", &data, &size));
    CHECK(assets_name(&v, 1) && strcmp(assets_name(&v, 1), "fonts/b.bin") == 0);
    CHECK(assets_name(&v, 2) == NULL);

    // The partition can be larger than the image, never smaller
    CHECK(assets_open(img, total, true, &v) == ASSETS_OK);
    CHECK(assets_open(img, total - 1, true, &v) == ASSETS_ERR_SIZE);

    uint8_t blank[64];
    memset(blank, 0xFF, sizeof(blank));
    CHECK(assets_open(blank, sizeof(blank), true, &v) == ASSETS_ERR_EMPTY);
    CHECK(assets_open(img, 8, true, &v) == ASSETS_ERR_EMPTY); // too short for a header

    uint8_t bad[512];
    memcpy(bad, img, sizeof(img));
    bad[0] = 'X';
    CHECK(assets_open(bad, sizeof(bad), true, &v) == ASSETS_ERR_MAGIC);

    memcpy(bad, img, sizeof(img));
    bad[4] = ASSETS_FORMAT + 1;
    CHECK(assets_open(bad, sizeof(bad), true, &v) == ASSETS_ERR_FORMAT);

    memcpy(bad, img, sizeof(img));
    bad[total - 1] ^= 1; // one flipped bit in a file
    CHECK(assets_open(bad, sizeof(bad), true, &v) == ASSETS_ERR_CRC);
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_OK); // structure is still fine

    memcpy(bad, img, sizeof(img));
    put32(bad + ASSETS_HEADER_LEN + ASSETS_ENTRY_LEN + 44, 21); // the second file runs one byte past the image
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_ERR_TABLE);

    memcpy(bad, img, sizeof(img));
    put32(bad + ASSETS_HEADER_LEN + 40, 40); // file data overlapping the table
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_ERR_TABLE);

    memcpy(bad, img, sizeof(img));
    memset(bad + ASSETS_HEADER_LEN, 'n', ASSETS_NAME_MAX); // name without its NUL
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_ERR_TABLE);

    memcpy(bad, img, sizeof(img));
    put32(bad + 8, 0x10000000); // a table larger than the image
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_ERR_TABLE);

    memcpy(bad, img, sizeof(img));
    put32(bad + 12, 0x7FFFFFF0); // total_len beyond the partition
    CHECK(assets_open(bad, sizeof(bad), false, &v) == ASSETS_ERR_SIZE);
}

// The image tools/pack_assets.py made from host_tests/vectors/assets/
static void packed_image_test(char const *path) {
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    static uint8_t img[1 << 16];
    size_t         len = fread(img, 1, sizeof(img), f);
    fclose(f);
    assets_view_t v;
    CHECK(assets_open(img, len, true, &v) == ASSETS_OK);
    CHECK(v.count == 2);
    uint8_t const *data;
    uint32_t       size;
    CHECK(assets_find(&v, "assets.toml", &data, &size) && size > 0 && memcmp(data, "# ", 2) == 0);
    CHECK(assets_find(&v, "fonts/sample.bin", &data, &size) && size == 37 && data[0] == 0 && data[36] == 36);
    CHECK(((uintptr_t)(data - img) % 16) == 0);
}

int main(int argc, char **argv) {
    unit_tests();
    if (argc > 1) {
        packed_image_test(argv[1]);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All assets_fmt tests passed\n");
    return 0;
}
