// Malformed-input helpers for parser tests (#211): truncation, bit flips, byte inserts (NUL, CR, LF), deletions,
// repeated ranges (a repeated field) and length-field lies, plus a seeded random mix of them. Header-only and
// deterministic, so a failing case is reproduced by its seed. Every mutation writes into a caller buffer and returns the
// new length; mut_exact() then copies the result into a heap block of exactly that size, so ASan reports any read past
// the length the parser was given.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t state;
} mut_rng_t;

static inline void mut_seed(mut_rng_t *r, uint32_t seed) {
    r->state = seed ? seed : 0x9e3779b9u; // xorshift32 never leaves 0
}

static inline uint32_t mut_next(mut_rng_t *r) {
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

// A number in [0, n); 0 when n is 0
static inline size_t mut_below(mut_rng_t *r, size_t n) {
    return n ? mut_next(r) % n : 0;
}

// The first cut bytes of src (all of it when cut >= len)
static inline size_t mut_truncate(uint8_t const *src, size_t len, size_t cut, uint8_t *dst, size_t cap) {
    size_t n = cut < len ? cut : len;
    n        = n < cap ? n : cap;
    memmove(dst, src, n);
    return n;
}

// Flip one bit of buf in place (bit counts from the first byte's lowest bit); out of range does nothing
static inline void mut_flip_bit(uint8_t *buf, size_t len, size_t bit) {
    if (bit / 8 < len) {
        buf[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
}

// src with n bytes of ins inserted at at; 0 when it does not fit cap
static inline size_t mut_insert(uint8_t const *src, size_t len, size_t at, void const *ins, size_t n, uint8_t *dst,
                                size_t cap) {
    if (at > len || n > cap || len > cap - n) {
        return 0;
    }
    memmove(dst, src, at);
    memcpy(dst + at, ins, n);
    memmove(dst + at + n, src + at, len - at);
    return len + n;
}

// src without the n bytes at at
static inline size_t mut_delete(uint8_t const *src, size_t len, size_t at, size_t n, uint8_t *dst, size_t cap) {
    if (at > len || n > len - at || len - n > cap) {
        return 0;
    }
    memmove(dst, src, at);
    memmove(dst + at, src + at + n, len - at - n);
    return len - n;
}

// src with its bytes [at, at + n) repeated right after themselves: a field (a header line, a TLV) sent twice
static inline size_t mut_repeat(uint8_t const *src, size_t len, size_t at, size_t n, uint8_t *dst, size_t cap) {
    if (at > len || n > len - at || n > cap || len > cap - n) {
        return 0;
    }
    memmove(dst, src, at + n);
    memcpy(dst + at + n, src + at, n);
    memmove(dst + at + 2 * n, src + at + n, len - at - n);
    return len + n;
}

// src with the first occurrence of needle replaced by repl (a text length field that lies); 0 when needle is absent
// or the result does not fit cap
static inline size_t mut_replace(uint8_t const *src, size_t len, char const *needle, char const *repl, uint8_t *dst,
                                 size_t cap) {
    size_t nl = strlen(needle);
    size_t rl = strlen(repl);
    for (size_t i = 0; nl && nl <= len && i <= len - nl; i++) {
        if (memcmp(src + i, needle, nl) == 0) {
            if (len - nl > cap || rl > cap - (len - nl)) {
                return 0;
            }
            memmove(dst, src, i);
            memcpy(dst + i, repl, rl);
            memmove(dst + i + rl, src + i + nl, len - i - nl);
            return len - nl + rl;
        }
    }
    return 0;
}

// A binary length field that lies: store value at at as width bytes (1, 2 or 4), little-endian, in place
static inline void mut_lie_le(uint8_t *buf, size_t len, size_t at, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width && at < len && i < len - at; i++) {
        buf[at + i] = (uint8_t)(value >> (8 * i));
    }
}

// A copy of buf in a heap block of exactly len bytes (1 when len is 0, never read): ASan catches any overread. Free it.
static inline uint8_t *mut_exact(uint8_t const *buf, size_t len) {
    uint8_t *p = malloc(len ? len : 1);
    if (p && len) {
        memcpy(p, buf, len);
    }
    return p;
}

// One random mutation of src into dst (cap bytes), returns the new length. The inserted bytes favour the ones that
// split text formats (NUL, CR, LF, ':', ' ', digits), so line and field parsers see their edge cases often. One case
// is a binary length lie: a 1-, 2- or 4-byte little-endian field at a random offset set to 0, 1, its maximum or a
// random value. A binary decoder test that knows where its length fields are calls mut_lie_le on them as well.
static inline size_t mut_random(mut_rng_t *r, uint8_t const *src, size_t len, uint8_t *dst, size_t cap) {
    static uint8_t const special[] = {0, '\r', '\n', ':', ' ', '\t', '0', '9', '-', 0xff, 0x80, '?', '='};
    size_t               at        = mut_below(r, len + 1);
    size_t               n;
    switch (mut_next(r) % 7) {
        case 0: return mut_truncate(src, len, at, dst, cap);
        case 1:
            n = mut_truncate(src, len, len, dst, cap);
            mut_flip_bit(dst, n, mut_below(r, n * 8));
            return n;
        case 2: {
            uint8_t b = special[mut_below(r, sizeof(special))];
            n         = mut_insert(src, len, at, &b, 1, dst, cap);
            return n ? n : mut_truncate(src, len, len, dst, cap);
        }
        case 3:
            n = mut_delete(src, len, at, 1 + mut_below(r, 4), dst, cap);
            return n ? n : mut_truncate(src, len, len, dst, cap);
        case 4:
            n = mut_repeat(src, len, at, mut_below(r, len - at + 1), dst, cap);
            return n ? n : mut_truncate(src, len, len, dst, cap);
        case 5:
            n = mut_truncate(src, len, len, dst, cap);
            if (n) {
                dst[mut_below(r, n)] = (uint8_t)mut_next(r);
            }
            return n;
        default: {
            static uint32_t const lies[] = {0, 1, 0xff, 0xffff, 0xffffffffu, 0x7fffffffu};
            unsigned const        width  = 1u << mut_below(r, 3);
            uint32_t const        value  = mut_below(r, 2) ? lies[mut_below(r, 6)] : mut_next(r);
            n                            = mut_truncate(src, len, len, dst, cap);
            mut_lie_le(dst, n, mut_below(r, n), width, value);
            return n;
        }
    }
}
