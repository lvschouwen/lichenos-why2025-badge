// Host test: a contact's texts (#85 hop count, #49 details)
#include "mesh_contact_fmt.h"

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

static char const *hops(uint8_t path_len) {
    static char out[16];
    mesh_format_hops(path_len, out, sizeof(out));
    return out;
}

// #85: the hash size rides in bits 6-7; the raw byte showed "65 hops" for one hop with 2-byte hashes
static void test_hops(void) {
    CHECK(strcmp(hops(0xFF), "flood") == 0);
    CHECK(strcmp(hops(0x00), "direct") == 0);
    CHECK(strcmp(hops(0x01), "1 hop") == 0);
    CHECK(strcmp(hops(0x03), "3 hops") == 0);
    CHECK(strcmp(hops(0x41), "1 hop") == 0);  // 2-byte hashes
    CHECK(strcmp(hops(0x82), "2 hops") == 0); // 3-byte hashes
    CHECK(strcmp(hops(0x40), "direct") == 0);
    mesh_path_t p = mesh_path_decode(0x41);
    CHECK(!p.flood && p.hops == 1 && p.hash_size == 2);
    p = mesh_path_decode(0xC5);
    CHECK(!p.flood && p.hops == 5 && p.hash_size == 4);
    p = mesh_path_decode(0x3F);
    CHECK(!p.flood && p.hops == 63 && p.hash_size == 1);
    p = mesh_path_decode(0xFF);
    CHECK(p.flood && p.hops == 0);
}

int main(void) {
    test_hops();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_contact_fmt_test: all passed\n");
    return 0;
}
