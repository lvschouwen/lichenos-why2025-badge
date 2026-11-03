// Host tests for the screenshot's run/literal compression (#146)
#include "ota_rle.h"

#include <stdbool.h>
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

// Encode in 8192-pixel slices like the server does; decode the concatenation; true when it equals the input
static bool round_trip(uint8_t const *in, size_t npx, size_t *enc_len) {
    enum { SLICE = 8192 };
    uint8_t *enc = malloc(ota_rle_bound(npx) + 16);
    uint8_t *dec = malloc(npx * 2 + 2);
    size_t   o = 0;
    for (size_t p = 0; p < npx; p += SLICE) {
        size_t n = npx - p < SLICE ? npx - p : SLICE;
        size_t w = ota_rle_encode(in + p * 2, n, enc + o);
        CHECK(w <= ota_rle_bound(n));
        o += w;
    }
    CHECK(o <= ota_rle_bound(npx));
    size_t got = ota_rle_decode(enc, o, dec, npx);
    bool   ok  = got == npx && memcmp(in, dec, npx * 2) == 0;
    *enc_len   = o;
    free(enc);
    free(dec);
    return ok;
}

static void put(uint8_t *f, size_t i, unsigned v) {
    f[2 * i]     = (uint8_t)v;
    f[2 * i + 1] = (uint8_t)(v >> 8);
}

// A 720x720 frame like the UI: flat background, bars, text-like noisy patches with a few antialiasing shades
static void ui_frame(uint8_t *f) {
    enum { W = 720, H = 720 };
    srand(7);
    for (size_t y = 0; y < H; y++) {
        for (size_t x = 0; x < W; x++) {
            unsigned v = 0x18C3;                      // background
            if (y < 60)                               v = 0x2965; // status bar
            else if (y % 80 < 4)                      v = 0x4A69; // row separator
            else if (y % 80 > 20 && y % 80 < 44 && x > 40 && x < 40 + (size_t)(y / 80 % 5 + 3) * 70 &&
                     (rand() % 3) == 0)               v = 0xFFFF - (unsigned)(rand() % 4) * 0x2104; // text glyph pixels
            put(f, y * W + x, v);
        }
    }
}

static void test_cases(void) {
    size_t  len;
    uint8_t one[2] = {1, 2};
    CHECK(round_trip(one, 1, &len) && len == 3);
    uint8_t zero[2];
    CHECK(ota_rle_encode(zero, 0, zero) == 0);
    CHECK(ota_rle_bound(0) == 0);

    size_t   n    = 200000; // longer than OTA_RLE_RUN_MAX
    uint8_t *flat = calloc(n, 2);
    CHECK(round_trip(flat, n, &len));
    CHECK(len < 200); // one 5-byte run per slice
    free(flat);

    // alternating pairs: runs of 2 stay literal
    uint8_t *alt = malloc(1000 * 2);
    for (size_t i = 0; i < 1000; i++) put(alt, i, (unsigned)(i / 2 % 2) * 7 + 1);
    CHECK(round_trip(alt, 1000, &len));
    free(alt);

    // run exactly at the short/long boundary and around it
    for (size_t r = 125; r <= 131; r++) {
        uint8_t buf[600] = {0};
        for (size_t i = 0; i < r; i++) put(buf, i, 9);
        put(buf, r, 5);
        put(buf, r + 1, 6);
        CHECK(round_trip(buf, r + 2, &len));
    }
}

static void test_noise(void) {
    size_t   n = 720 * 720, len;
    uint8_t *f = malloc(n * 2);
    srand(1);
    for (size_t i = 0; i < n * 2; i++) f[i] = (uint8_t)rand();
    CHECK(round_trip(f, n, &len));
    CHECK(len <= ota_rle_bound(n));
    printf("noise: %zu -> %zu bytes (bound %zu)\n", n * 2, len, ota_rle_bound(n));
    free(f);
}

static void test_ui(void) {
    size_t   n = 720 * 720, len;
    uint8_t *f = malloc(n * 2);
    ui_frame(f);
    CHECK(round_trip(f, n, &len));
    printf("ui-like: %zu -> %zu bytes (%.1f:1)\n", n * 2, len, (double)(n * 2) / (double)len);
    CHECK(len * 4 < n * 2);
    free(f);
}

static void test_bad(void) {
    uint8_t out[8];
    uint8_t lit[] = {1, 0, 0}; // promises 2 pixels, holds 1
    CHECK(ota_rle_decode(lit, sizeof(lit), out, 4) == SIZE_MAX);
    uint8_t run[] = {0x83, 1, 2}; // 4 pixels into room for 2
    CHECK(ota_rle_decode(run, sizeof(run), out, 2) == SIZE_MAX);
    uint8_t cut[] = {0xFF, 1}; // long run header cut short
    CHECK(ota_rle_decode(cut, sizeof(cut), out, 4) == SIZE_MAX);
}

int main(void) {
    test_cases();
    test_noise();
    test_ui();
    test_bad();
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ota_rle_test ok\n");
    return 0;
}
