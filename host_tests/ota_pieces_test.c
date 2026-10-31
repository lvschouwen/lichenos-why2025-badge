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

typedef struct {
    uint8_t const *mem;
    size_t         len;
    size_t         max_chunk; // the biggest read seen
} fake_t;

static int fake_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    fake_t *f = ctx;
    if (off + n > f->len) {
        return -1;
    }
    if (n > f->max_chunk) {
        f->max_chunk = n;
    }
    memcpy(buf, f->mem + off, n);
    return 0;
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

    // The same checks through a chunked reader (the OTA slot): small reads, same answers
    fake_t  f = {data, sizeof(data), 0};
    uint8_t scratch[64];
    cnt = ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4);
    CHECK(cnt == 2 && bad[0] == 2 && bad[1] == 3);
    CHECK(f.max_chunk == sizeof(scratch));
    data[650] ^= 0x01;
    data[999] ^= 0x80;
    CHECK(ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4) == 0);
    f.len = 700; // pieces 2 and 3 can't be read: they count as differing
    CHECK(ota_pieces_check_read(&p, fake_read, &f, scratch, sizeof(scratch), bad, 4) == 2);
    f.len = sizeof(data);
    uint8_t whole[32], expect[32];
    CHECK(ota_hash_range(fake_read, &f, 0, sizeof(data), scratch, sizeof(scratch), whole) == 0);
    hash(data, sizeof(data), expect);
    CHECK(memcmp(whole, expect, 32) == 0);
    CHECK(ota_hash_range(fake_read, &f, 10, sizeof(data), scratch, sizeof(scratch), whole) == -1);

    // Flash pieces: sector-aligned start; whole sectors long unless last
    CHECK(ota_piece_flash_ok(0, 131072, 1784656, 4096));
    CHECK(ota_piece_flash_ok(1703936, 80720, 1784656, 4096)); // the last, ragged piece
    CHECK(!ota_piece_flash_ok(1703936, 80000, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(100, 4096, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(0, 5000, 1784656, 4096));
    CHECK(!ota_piece_flash_ok(0, 0, 1784656, 4096));

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
