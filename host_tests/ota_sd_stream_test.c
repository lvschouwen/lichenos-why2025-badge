#include "ota_sd_stream.h"

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

// Receive one piece the way the server does: hash into the candidate, accept when it is whole
static bool piece(ota_sd_stream_t *s, uint8_t const *data, size_t off, size_t len, bool arrives_whole) {
    sha256_ctx_t *c = ota_sd_stream_piece(s, off);
    if (!c) {
        return false;
    }
    sha256_update(c, data + off, arrives_whole ? len : len / 2);
    return arrives_whole && ota_sd_stream_accept(s, len);
}

int main(void) {
    // The cap is the free space, not the 4 MB the PSRAM stage allows
    CHECK(ota_sd_cap(true, 28ULL * 1024 * 1024 + 5) == 28LL * 1024 * 1024 + 5);
    CHECK(ota_sd_cap(true, 0) == 0);
    CHECK(ota_sd_cap(false, 123) == OTA_SD_TOTAL_MAX);
    CHECK(ota_sd_cap(true, UINT64_MAX) == OTA_SD_TOTAL_MAX);
    CHECK(ota_sd_cap(true, 29ULL * 1024 * 1024 * 1024) > 4LL * 1024 * 1024);

    CHECK(ota_sd_piece_ok(1) && ota_sd_piece_ok(OTA_SD_PIECE_MAX));
    CHECK(!ota_sd_piece_ok(0) && !ota_sd_piece_ok(-1) && !ota_sd_piece_ok(OTA_SD_PIECE_MAX + 1));

    static uint8_t data[1000];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 7 + 3);
    }
    uint8_t want[32], got[32];
    hash(data, sizeof(data), want);

    ota_sd_stream_t s;
    ota_sd_stream_begin(&s, sizeof(data));
    CHECK(!ota_sd_stream_final(&s, got)); // nothing received yet
    CHECK(piece(&s, data, 0, 300, true));
    // A piece that stopped half-way, then comes again: its first half is not hashed twice
    CHECK(!piece(&s, data, 300, 300, false));
    CHECK(s.got == 300);
    CHECK(piece(&s, data, 300, 300, true));
    // The same piece sent again after it was accepted is out of order: refused, running hash untouched
    CHECK(ota_sd_stream_piece(&s, 300) == NULL);
    CHECK(ota_sd_stream_piece(&s, 900) == NULL);
    CHECK(!ota_sd_stream_final(&s, got)); // not the whole file yet
    // A piece running past the file's end is not counted
    sha256_ctx_t *c = ota_sd_stream_piece(&s, 600);
    CHECK(c != NULL);
    sha256_update(c, data + 600, 400);
    CHECK(!ota_sd_stream_accept(&s, 401));
    CHECK(s.got == 600);
    CHECK(!ota_sd_stream_accept(&s, 0));
    CHECK(piece(&s, data, 600, 400, true));
    CHECK(ota_sd_stream_final(&s, got));
    CHECK(memcmp(got, want, 32) == 0);
    // final() is repeatable: it works on a copy
    CHECK(ota_sd_stream_final(&s, got) && memcmp(got, want, 32) == 0);

    // A wrong byte shows in the whole-file hash
    ota_sd_stream_begin(&s, sizeof(data));
    data[10] ^= 1;
    CHECK(piece(&s, data, 0, 500, true));
    data[10] ^= 1;
    CHECK(piece(&s, data, 500, 500, true));
    CHECK(ota_sd_stream_final(&s, got) && memcmp(got, want, 32) != 0);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_sd_stream_test: all checks passed\n");
    return 0;
}
