#include "psram_pattern.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    enum { N = 4096 };
    static uint8_t storage[N] __attribute__((aligned(PSRAM_LINE)));
    uint8_t       *buf = storage;
    uintptr_t tag = 0x48000000u; // a PSRAM-like, line-aligned address

    psram_pattern_fill(buf, N, tag);
    psram_check_t r = psram_pattern_check(buf, N, tag);
    CHECK(r.bad_bytes == 0 && r.bad_lines == 0 && r.bit_flips == 0);
    for (size_t i = 0; i < N; i++) {
        CHECK(buf[i] == psram_pattern_byte(tag + i));
    }

    // Address-dependent: the same bytes at another address do not pass
    r = psram_pattern_check(buf, N, tag + 128);
    CHECK(r.bad_bytes > N / 2);
    // Not a constant or repeating fill
    int distinct[256] = {0}, kinds = 0;
    for (size_t i = 0; i < N; i++) {
        if (!distinct[buf[i]]++) {
            kinds++;
        }
    }
    CHECK(kinds > 200);

    // One flipped bit: one byte, one single-bit word, one line, offset reported
    buf[1000] ^= 0x10;
    r = psram_pattern_check(buf, N, tag);
    CHECK(r.bad_bytes == 1 && r.first_bad == 1000 && r.bit_flips == 1 && r.bad_lines == 1 && r.full_lines == 0);
    CHECK(r.first_xor == (0x10u << (8 * (1000 % 4))));
    buf[1000] ^= 0x10;

    // Two bytes in one word: not a single-bit flip; first bad is the lower one
    buf[64] ^= 0xff;
    buf[65] ^= 0x01;
    r = psram_pattern_check(buf, N, tag);
    CHECK(r.bad_bytes == 2 && r.first_bad == 64 && r.bit_flips == 0 && r.bad_lines == 1);
    psram_pattern_fill(buf, N, tag);

    // A whole 128 B line wiped, and half of the next one
    memset(buf + 256, 0, 128);
    memset(buf + 384, 0xAA, 64);
    r = psram_pattern_check(buf, N, tag);
    CHECK(r.bad_lines == 2 && r.full_lines == 1 && r.first_bad >= 256 && r.first_bad < 260);
    CHECK(r.bad_bytes <= 192 && r.bad_bytes > 150);
    psram_pattern_fill(buf, N, tag);

    // Odd lengths and tags: head and tail go byte by byte
    uint8_t odd[37];
    psram_pattern_fill(odd, sizeof(odd), tag + 3);
    r = psram_pattern_check(odd, sizeof(odd), tag + 3);
    CHECK(r.bad_bytes == 0);
    odd[0] ^= 1;
    odd[36] ^= 0x80;
    r = psram_pattern_check(odd, sizeof(odd), tag + 3);
    CHECK(r.bad_bytes == 2 && r.first_bad == 0);

    CHECK(psram_pattern_check(buf, 0, tag).bad_bytes == 0);

    // Inverted pass: every word XORed; it fails the plain check and passes its own, a single flip is still seen
    psram_pattern_fill_x(buf, N, tag, 0xFFFFFFFFu);
    CHECK(psram_pattern_check(buf, N, tag).bad_bytes == N);
    CHECK(psram_pattern_check_x(buf, N, tag, 0xFFFFFFFFu).bad_bytes == 0);
    buf[8] ^= 0x01;
    r = psram_pattern_check_x(buf, N, tag, 0xFFFFFFFFu);
    CHECK(r.bad_bytes == 1 && r.bit_flips == 1 && r.first_bad == 8);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_pattern_test ok\n");
    return 0;
}
