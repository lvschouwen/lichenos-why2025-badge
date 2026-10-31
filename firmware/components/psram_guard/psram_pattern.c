#include "psram_pattern.h"

#include <string.h>

// One word per aligned 4 bytes, so fill and check can run a word at a time
static uint32_t word_at(uintptr_t addr) {
    uint32_t x = (uint32_t)addr * 0x9E3779B1u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x;
}

static uint8_t byte_x(uintptr_t addr, uint32_t mask) {
    return (uint8_t)((word_at(addr & ~(uintptr_t)3) ^ mask) >> (8 * (addr & 3)));
}

uint8_t psram_pattern_byte(uintptr_t addr) {
    return byte_x(addr, 0);
}

void psram_pattern_fill_x(void *buf, size_t len, uintptr_t tag, uint32_t mask) {
    uint8_t *p = buf;
    size_t   i = 0;
    for (; i < len && ((tag + i) & 3); i++) {
        p[i] = byte_x(tag + i, mask);
    }
    for (; i + 4 <= len; i += 4) {
        uint32_t w = (word_at(tag + i) ^ mask);
        memcpy(p + i, &w, 4); // little-endian, like psram_pattern_byte
    }
    for (; i < len; i++) {
        p[i] = byte_x(tag + i, mask);
    }
}

// Records a differing byte at offset at; word_diff = the difference placed at its byte lane of the 32-bit word, up/down =
// the bits found set that should be clear / found clear that should be set (same lane)
static void note_bad(psram_check_t *res, size_t at, uint32_t word_diff, uint32_t first_xor, uint32_t up, uint32_t down) {
    if (res->bad_bytes++ == 0) {
        res->first_bad = at;
        res->first_xor = first_xor;
    }
    res->last_bad  = at;
    res->xor_or   |= word_diff;
    res->up_or    |= up;
    res->down_or  |= down;
}

static void note_bad_byte(psram_check_t *res, uintptr_t tag, size_t i, uint8_t got, uint32_t mask) {
    uint8_t  want = byte_x(tag + i, mask);
    unsigned sh   = 8 * ((tag + i) & 3);
    note_bad(res, i, (uint32_t)(got ^ want) << sh, got ^ want, (uint32_t)(got & ~want & 0xffu) << sh,
             (uint32_t)(want & ~got & 0xffu) << sh);
}

psram_check_t psram_pattern_check_x(void const *buf, size_t len, uintptr_t tag, uint32_t mask) {
    uint8_t const *p   = buf;
    psram_check_t  res = {0};
    size_t         i   = 0;
    for (; i < len && ((tag + i) & 3); i++) { // unaligned head and tail: bytes only
        if (p[i] != byte_x(tag + i, mask)) {
            note_bad_byte(&res, tag, i, p[i], mask);
        }
    }
    uintptr_t line      = (uintptr_t)-1;
    size_t    line_bad  = 0;
    size_t    line_seen = 0;
    for (; i + 4 <= len; i += 4) {
        uintptr_t l = (tag + i) / PSRAM_LINE;
        if (l != line) {
            if (line_seen && line_bad == PSRAM_LINE / 4) {
                res.full_lines++;
            }
            line     = l;
            line_bad = 0;
            line_seen = 1;
        }
        uint32_t got;
        memcpy(&got, p + i, 4);
        uint32_t diff = got ^ (word_at(tag + i) ^ mask);
        if (!diff) {
            continue;
        }
        if (line_bad++ == 0) {
            res.bad_lines++;
        }
        if ((diff & (diff - 1)) == 0) {
            res.bit_flips++;
        }
        for (size_t k = 0; k < 4; k++) {
            if ((diff >> (8 * k)) & 0xff) {
                uint32_t want = word_at(tag + i) ^ mask;
                uint32_t lane = 0xffu << (8 * k);
                note_bad(&res, i + k, diff & lane, diff, got & ~want & lane, want & ~got & lane);
            }
        }
    }
    if (line_seen && line_bad == PSRAM_LINE / 4) {
        res.full_lines++;
    }
    for (; i < len; i++) {
        if (p[i] != byte_x(tag + i, mask)) {
            note_bad_byte(&res, tag, i, p[i], mask);
        }
    }
    return res;
}

void psram_pattern_fill(void *buf, size_t len, uintptr_t tag) {
    psram_pattern_fill_x(buf, len, tag, 0);
}

psram_check_t psram_pattern_check(void const *buf, size_t len, uintptr_t tag) {
    return psram_pattern_check_x(buf, len, tag, 0);
}
