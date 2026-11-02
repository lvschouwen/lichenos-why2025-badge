#include "psram_dma.h"

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

enum { N = 8192, CHUNK = 1024 };
static uint8_t buf[N] __attribute__((aligned(128)));
static uint8_t chunk[CHUNK] __attribute__((aligned(128)));

// A clean fill checks clean, whole or chunk by chunk from any offset, and the fill is byte-addressable by offset
static void test_roundtrip(psram_dma_pattern_t p) {
    psram_dma_report_t r;
    psram_dma_fill(buf, N, p, 0x1234u, 0);
    psram_dma_report_reset(&r);
    psram_dma_compare(buf, N, p, 0x1234u, 0, &r);
    CHECK(r.bad_bytes == 0 && r.kept == 0 && r.same_mod1k == 0);
    for (size_t off = 0; off < N; off += CHUNK) {
        psram_dma_fill(chunk, CHUNK, p, 0x1234u, off);
        CHECK(memcmp(chunk, buf + off, CHUNK) == 0);
        psram_dma_compare(buf + off, CHUNK, p, 0x1234u, off, &r);
    }
    CHECK(r.bad_bytes == 0);
    // a word's byte k is the byte at off + k
    uint32_t w = psram_dma_word(p, 0x1234u, 100);
    CHECK((uint8_t)w == buf[100] && (uint8_t)(w >> 24) == buf[103]);
    // the fill is not constant (walking patterns and the address pattern included)
    int same = 1;
    for (size_t i = 1; i < 64; i++) {
        same = same && buf[i] == buf[0];
    }
    CHECK(!same);
}

static void test_patterns(void) {
    for (int p = 0; p < PSRAM_DMA_PATTERNS; p++) {
        test_roundtrip((psram_dma_pattern_t)p);
        CHECK(strcmp(psram_dma_pattern_name((psram_dma_pattern_t)p), "?") != 0);
    }
    // walking ones / zeros: one bit per byte, every bit position in every 8 bytes
    psram_dma_fill(buf, 16, PSRAM_DMA_WALK1, 0, 0);
    CHECK(buf[0] == 0x01 && buf[3] == 0x08 && buf[7] == 0x80 && buf[8] == 0x01);
    psram_dma_fill(buf, 16, PSRAM_DMA_WALK0, 0, 0);
    CHECK(buf[0] == 0xFE && buf[3] == 0xF7);
    // inverse really inverts, and chunk generation from a 4-byte offset agrees with the whole
    psram_dma_fill(buf, 256, PSRAM_DMA_ADDR, 0, 1024);
    psram_dma_fill(chunk, 256, PSRAM_DMA_ADDR_INV, 0, 1024);
    for (size_t i = 0; i < 256; i++) {
        CHECK((uint8_t)~buf[i] == chunk[i]);
    }
    // random depends on the seed
    psram_dma_fill(buf, 256, PSRAM_DMA_RANDOM, 1, 0);
    psram_dma_fill(chunk, 256, PSRAM_DMA_RANDOM, 2, 0);
    CHECK(memcmp(buf, chunk, 256) != 0);
    // journal: the #120 bytes sit at 859 mod 1024, cycling through both pairs; the rest is random
    psram_dma_fill(buf, N, PSRAM_DMA_JOURNAL, 7, 0);
    CHECK(buf[859] == 0x11 && buf[1883] == 0x19 && buf[2907] == 0x17 && buf[3931] == 0x1f && buf[4955] == 0x11);
    psram_dma_fill(chunk, CHUNK, PSRAM_DMA_RANDOM, 7, 0);
    CHECK(memcmp(buf, chunk, 859) == 0 && memcmp(buf + 860, chunk + 860, CHUNK - 860) == 0);
}

// The #120 damage: bit 3 flipped at 859 mod 1024 in several KB, reported with offsets, histogram and the fixed spot
static void test_report(void) {
    psram_dma_report_t r;
    psram_dma_fill(buf, N, PSRAM_DMA_JOURNAL, 3, 0);
    for (size_t o = 859; o < N; o += 1024) {
        buf[o] ^= 0x08;
    }
    buf[5000] ^= 0x81; // and one two-bit error elsewhere
    psram_dma_report_reset(&r);
    // checked in two chunks with a logical base offset, as the ESP side does
    psram_dma_compare(buf, N / 2, PSRAM_DMA_JOURNAL, 3, 0, &r);
    psram_dma_compare(buf + N / 2, N / 2, PSRAM_DMA_JOURNAL, 3, N / 2, &r);
    CHECK(r.bad_bytes == 9);
    CHECK(r.bit_hist[3] == 8 && r.bit_hist[0] == 1 && r.bit_hist[7] == 1 && r.bit_hist[1] == 0);
    CHECK(r.same_mod1k == 8);
    CHECK(r.kept == 8); // PSRAM_DMA_KEEP: the ninth is only counted
    CHECK(r.flips[0].off == 859 && r.flips[0].want == 0x11 && r.flips[0].got == 0x19);
    CHECK(r.flips[4].off == 4955 && r.flips[4].want == 0x11 && r.flips[4].got == 0x19);
    CHECK(r.flips[7].off == 7003);

    char text[96];
    size_t n = psram_dma_flip_text(&r.flips[4], text, sizeof(text));
    CHECK(n == strlen(text));
    CHECK(strcmp(text, "+4955 (859 mod 1024) want 11 got 19 xor 08") == 0);
    n = psram_dma_hist_text(&r, text, sizeof(text));
    CHECK(n == strlen(text));
    CHECK(strcmp(text, "bits 0:1 1:0 2:0 3:8 4:0 5:0 6:0 7:1, 8 of 9 at the same offset mod 1024") == 0);
    // a short buffer is cut, not overrun
    char small[20];
    n = psram_dma_hist_text(&r, small, sizeof(small));
    CHECK(n == 19 && small[19] == 0);

    // a multi-byte difference inside one word: every byte counted once, bits per byte
    psram_dma_report_reset(&r);
    psram_dma_fill(buf, 16, PSRAM_DMA_WALK1, 0, 0);
    buf[4] ^= 0xFF;
    buf[5] ^= 0x01;
    psram_dma_compare(buf, 16, PSRAM_DMA_WALK1, 0, 0, &r);
    CHECK(r.bad_bytes == 2 && r.bit_hist[0] == 2 && r.bit_hist[7] == 1 && r.flips[0].off == 4 && r.flips[1].off == 5);
    CHECK(r.same_mod1k == 1);
}

// A copy compared with a reference copy: same reporting, offsets carry the logical base
static void test_compare_buf(void) {
    psram_dma_report_t r;
    psram_dma_fill(buf, 2048, PSRAM_DMA_RANDOM, 9, 0);
    memcpy(chunk, buf + 1024, 1024);
    psram_dma_report_reset(&r);
    psram_dma_compare_buf(chunk, buf + 1024, 1024, 1024, &r);
    CHECK(r.bad_bytes == 0);
    chunk[859] ^= 0x08;
    chunk[3] ^= 0x80;
    psram_dma_compare_buf(chunk, buf + 1024, 1024, 1024, &r);
    CHECK(r.bad_bytes == 2 && r.bit_hist[3] == 1 && r.bit_hist[7] == 1);
    CHECK(r.flips[0].off == 1027 && r.flips[1].off == 1883 && r.flips[1].got == (uint8_t)(buf[1883] ^ 0x08));
}

// The sweep leaves a fixed reserve of the free PSRAM and refuses when too few windows fit
static void test_sweep_windows(void) {
    size_t const mb = 1024u * 1024;
    CHECK(psram_dma_sweep_windows(20 * mb, 24 * mb, 8 * mb, mb, 4) == 16);       // the reserve, not the block, limits
    CHECK(psram_dma_sweep_windows(10 * mb, 24 * mb, 8 * mb, mb, 4) == 10);       // the largest block limits
    CHECK(psram_dma_sweep_windows(20 * mb, 11 * mb, 8 * mb, mb, 4) == 0);        // 3 windows: below the minimum
    CHECK(psram_dma_sweep_windows(20 * mb, 12 * mb + 5, 8 * mb, mb, 4) == 4);    // exactly the minimum
    CHECK(psram_dma_sweep_windows(20 * mb, 8 * mb, 8 * mb, mb, 1) == 0);         // nothing above the reserve
    CHECK(psram_dma_sweep_windows(20 * mb, 4 * mb, 8 * mb, mb, 1) == 0);
    CHECK(psram_dma_sweep_windows(20 * mb, 24 * mb, 8 * mb, 0, 1) == 0);
}

int main(void) {
    test_patterns();
    test_sweep_windows();
    test_report();
    test_compare_buf();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("psram_dma_test OK\n");
    return 0;
}
