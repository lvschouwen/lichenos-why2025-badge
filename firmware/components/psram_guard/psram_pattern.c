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

uint8_t psram_pattern_byte(uintptr_t addr) {
    return (uint8_t)(word_at(addr & ~(uintptr_t)3) >> (8 * (addr & 3)));
}

void psram_pattern_fill(void *buf, size_t len, uintptr_t tag) {
    uint8_t *p = buf;
    size_t   i = 0;
    for (; i < len && ((tag + i) & 3); i++) {
        p[i] = psram_pattern_byte(tag + i);
    }
    for (; i + 4 <= len; i += 4) {
        uint32_t w = word_at(tag + i);
        memcpy(p + i, &w, 4); // little-endian, like psram_pattern_byte
    }
    for (; i < len; i++) {
        p[i] = psram_pattern_byte(tag + i);
    }
}

psram_check_t psram_pattern_check(void const *buf, size_t len, uintptr_t tag) {
    uint8_t const *p   = buf;
    psram_check_t  res = {0};
    size_t         i   = 0;
    for (; i < len && ((tag + i) & 3); i++) { // unaligned head and tail: bytes only
        if (p[i] != psram_pattern_byte(tag + i)) {
            if (res.bad_bytes++ == 0) {
                res.first_bad = i;
                res.first_xor = p[i] ^ psram_pattern_byte(tag + i);
            }
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
        uint32_t diff = got ^ word_at(tag + i);
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
                if (res.bad_bytes++ == 0) {
                    res.first_bad = i + k;
                    res.first_xor = diff;
                }
            }
        }
    }
    if (line_seen && line_bad == PSRAM_LINE / 4) {
        res.full_lines++;
    }
    for (; i < len; i++) {
        if (p[i] != psram_pattern_byte(tag + i)) {
            if (res.bad_bytes++ == 0) {
                res.first_bad = i;
                res.first_xor = p[i] ^ psram_pattern_byte(tag + i);
            }
        }
    }
    return res;
}
