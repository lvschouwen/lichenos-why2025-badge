#include "ota_pieces.h"
#include "sha256.h"

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

static void hash(uint8_t const *d, size_t n, uint8_t out[32]) {
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, d, n);
    sha256_final(&c, out);
}

int main(void) {
    CHECK(ota_round_up(0, 128) == 0);
    CHECK(ota_round_up(1, 128) == 128);
    CHECK(ota_round_up(128, 128) == 128);
    CHECK(ota_round_up(129, 128) == 256);
    CHECK(ota_round_up(1000001, 128) == 1000064);

    uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    ota_pieces_t p = {0};
    uint8_t      d[32];
    size_t       bad[4];
    for (size_t off = 0; off < sizeof(data); off += 300) {
        size_t n = sizeof(data) - off < 300 ? sizeof(data) - off : 300;
        hash(data + off, n, d);
        CHECK(ota_pieces_add(&p, off, n, d));
    }
    CHECK(p.n == 4);
    CHECK(ota_pieces_check(&p, data, bad, 4) == 0);

    data[650] ^= 0x01; // piece 2 (600..899)
    data[999] ^= 0x80; // piece 3
    size_t cnt = ota_pieces_check(&p, data, bad, 1);
    CHECK(cnt == 2);
    CHECK(bad[0] == 2);
    cnt = ota_pieces_check(&p, data, bad, 4);
    CHECK(cnt == 2 && bad[0] == 2 && bad[1] == 3);

    ota_pieces_clear(&p);
    CHECK(p.n == 0 && p.v == NULL);
    for (int i = 0; i < OTA_PIECES_MAX; i++) {
        CHECK(ota_pieces_add(&p, 0, 1, d));
    }
    CHECK(!ota_pieces_add(&p, 0, 1, d));
    ota_pieces_clear(&p);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_pieces_test: all checks passed\n");
    return 0;
}
