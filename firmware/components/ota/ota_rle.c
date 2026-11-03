#include "ota_rle.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define LIT_MAX   128
#define RUN_MIN   3 // a run of 2 saves one byte but splits a literal (one more control byte)
#define RUN_SHORT 127

size_t ota_rle_bound(size_t npx) {
    return npx * 2 + (npx + LIT_MAX - 1) / LIT_MAX;
}

static bool same(uint8_t const *a, uint8_t const *b) {
    return a[0] == b[0] && a[1] == b[1];
}

static size_t run_len(uint8_t const *in, size_t i, size_t npx) {
    size_t n = 1;
    while (i + n < npx && n < OTA_RLE_RUN_MAX && same(in + 2 * i, in + 2 * (i + n))) {
        n++;
    }
    return n;
}

static size_t put_run(uint8_t *out, size_t n, uint8_t const *px) {
    size_t o = 0;
    if (n <= RUN_SHORT) {
        out[o++] = (uint8_t)(0x80 | (n - 1));
    } else {
        out[o++] = 0xFF;
        out[o++] = (uint8_t)((n - 128) & 0xFF);
        out[o++] = (uint8_t)((n - 128) >> 8);
    }
    out[o++] = px[0];
    out[o++] = px[1];
    return o;
}

static size_t put_literals(uint8_t *out, uint8_t const *px, size_t n) {
    size_t o = 0;
    while (n) {
        size_t k = n < LIT_MAX ? n : LIT_MAX;
        out[o++] = (uint8_t)(k - 1);
        memcpy(out + o, px, k * 2);
        o  += k * 2;
        px += k * 2;
        n  -= k;
    }
    return o;
}

size_t ota_rle_encode(uint8_t const *in, size_t npx, uint8_t *out) {
    size_t o = 0, lit = 0; // lit: start of the pending literal pixels
    size_t i = 0;
    while (i < npx) {
        size_t n = run_len(in, i, npx);
        if (n >= RUN_MIN) {
            o += put_literals(out + o, in + 2 * lit, i - lit);
            o += put_run(out + o, n, in + 2 * i);
            lit = i += n;
        } else {
            i += n;
        }
    }
    return o + put_literals(out + o, in + 2 * lit, npx - lit);
}

size_t ota_rle_decode(uint8_t const *in, size_t len, uint8_t *out, size_t npx) {
    size_t i = 0, got = 0;
    while (i < len) {
        uint8_t c = in[i++];
        size_t  n;
        if (c < 0x80) {
            n = (size_t)c + 1;
            if (i + n * 2 > len || got + n > npx) {
                return SIZE_MAX;
            }
            memcpy(out + got * 2, in + i, n * 2);
            i += n * 2;
        } else {
            if (c == 0xFF) {
                if (i + 2 > len) {
                    return SIZE_MAX;
                }
                n  = 128 + ((size_t)in[i] | (size_t)in[i + 1] << 8);
                i += 2;
            } else {
                n = (size_t)(c & 0x7F) + 1;
            }
            if (i + 2 > len || got + n > npx) {
                return SIZE_MAX;
            }
            for (size_t k = 0; k < n; k++) {
                memcpy(out + (got + k) * 2, in + i, 2);
            }
            i += 2;
        }
        got += n;
    }
    return got;
}
