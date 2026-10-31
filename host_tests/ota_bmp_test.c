// Host tests for the screenshot's BMP header (#30)
#include "ota_bmp.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static uint32_t u32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_layout(void) {
    uint8_t h[OTA_BMP_HEADER_LEN];
    CHECK(ota_bmp565_header(720, 720, h));
    CHECK(h[0] == 'B' && h[1] == 'M');
    CHECK(u32(h + 2) == 66 + 1440u * 720); // file size
    CHECK(u32(h + 10) == 66);              // pixel data offset
    CHECK(u32(h + 14) == 40);
    CHECK(u32(h + 18) == 720);
    CHECK((int32_t)u32(h + 22) == -720); // top-down
    CHECK(h[26] == 1 && h[27] == 0 && h[28] == 16 && h[29] == 0);
    CHECK(u32(h + 30) == 3); // BI_BITFIELDS
    CHECK(u32(h + 34) == 1440u * 720);
    CHECK(u32(h + 54) == 0xF800 && u32(h + 58) == 0x07E0 && u32(h + 62) == 0x001F);
    CHECK(u32(h + 38) == 0 && u32(h + 46) == 0); // no palette
}

static void test_sizes(void) {
    CHECK(ota_bmp565_row_bytes(720) == 1440);
    CHECK(ota_bmp565_row_bytes(3) == 8); // rows pad to 4 bytes
    CHECK(ota_bmp565_row_bytes(1) == 4);
    CHECK(ota_bmp565_file_size(2, 5) == 66 + 4 * 5);
    uint8_t h[OTA_BMP_HEADER_LEN];
    CHECK(!ota_bmp565_header(0, 10, h));
    CHECK(!ota_bmp565_header(10, 0, h));
    CHECK(!ota_bmp565_header(1000000, 10, h));
    CHECK(ota_bmp565_file_size(0, 5) == 0);
}

int main(void) {
    test_layout();
    test_sizes();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ota_bmp_test ok\n");
    return 0;
}
