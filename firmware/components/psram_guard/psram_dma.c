#include "psram_dma.h"

#include "psram_pattern.h"

#include <stdio.h>
#include <string.h>

#define ADDR_TAG      0x48000000u // a PSRAM-like base for the address pattern: the logical offset is added
#define JOURNAL_SPOT  859         // #120: bit 3 flipped at file offsets 859 mod 1024, values 0x11/0x19 and 0x17/0x1f

static char const *const names[PSRAM_DMA_PATTERNS] = {"addr", "~addr", "walk1", "walk0", "random", "journal"};

char const *psram_dma_pattern_name(psram_dma_pattern_t p) {
    return (unsigned)p < PSRAM_DMA_PATTERNS ? names[p] : "?";
}

// splitmix-style mix of the seed and the word index: every word differs, no period inside a 32 MB buffer
static uint32_t mix(uint32_t seed, uint32_t index) {
    uint32_t z = index * 0x9E3779B9u + seed;
    z          = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z          = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

static uint32_t addr_word(size_t off) {
    uint32_t w = 0;
    for (int k = 3; k >= 0; k--) {
        w = (w << 8) | psram_pattern_byte(ADDR_TAG + off + (size_t)k);
    }
    return w;
}

static uint32_t walk_word(size_t off) {
    uint32_t w = 0;
    for (int k = 3; k >= 0; k--) {
        w = (w << 8) | (uint32_t)(1u << ((off + (size_t)k) % 8));
    }
    return w;
}

// The journal pattern plants the two byte pairs seen in #120 at the spot where they flipped, in both directions
static uint32_t journal_word(uint32_t seed, size_t off) {
    static uint8_t const spot[4] = {0x11, 0x19, 0x17, 0x1f};
    uint32_t             w       = mix(seed, (uint32_t)(off >> 2));
    for (size_t k = 0; k < 4; k++) {
        size_t o = off + k;
        if (o % 1024 == JOURNAL_SPOT) {
            w = (w & ~(0xFFu << (8 * k))) | ((uint32_t)spot[(o / 1024) % 4] << (8 * k));
        }
    }
    return w;
}

uint32_t psram_dma_word(psram_dma_pattern_t p, uint32_t seed, size_t off) {
    switch (p) {
    case PSRAM_DMA_ADDR:
        return addr_word(off);
    case PSRAM_DMA_ADDR_INV:
        return ~addr_word(off);
    case PSRAM_DMA_WALK1:
        return walk_word(off);
    case PSRAM_DMA_WALK0:
        return ~walk_word(off);
    case PSRAM_DMA_RANDOM:
        return mix(seed, (uint32_t)(off >> 2));
    case PSRAM_DMA_JOURNAL:
        return journal_word(seed, off);
    default:
        return 0;
    }
}

void psram_dma_fill(void *buf, size_t len, psram_dma_pattern_t p, uint32_t seed, size_t off) {
    uint8_t *out = buf;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t w = psram_dma_word(p, seed, off + i);
        memcpy(out + i, &w, 4); // little-endian target and host: byte k of the word is the byte at off + i + k
    }
}

void psram_dma_report_reset(psram_dma_report_t *r) {
    memset(r, 0, sizeof(*r));
}

static void note_flip(psram_dma_report_t *r, size_t off, uint8_t want, uint8_t got) {
    uint8_t x = want ^ got;
    if (r->bad_bytes == 0 || off % 1024 == r->flips[0].off % 1024) {
        r->same_mod1k++;
    }
    r->bad_bytes++;
    for (int b = 0; b < 8; b++) {
        if (x & (1u << b)) {
            r->bit_hist[b]++;
        }
    }
    if (r->kept < PSRAM_DMA_KEEP) {
        r->flips[r->kept++] = (psram_dma_flip_t){.off = off, .want = want, .got = got};
    }
}

void psram_dma_compare(void const *got, size_t len, psram_dma_pattern_t p, uint32_t seed, size_t off, psram_dma_report_t *r) {
    uint8_t const *in = got;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t want = psram_dma_word(p, seed, off + i);
        uint32_t have;
        memcpy(&have, in + i, 4);
        if (have == want) {
            continue;
        }
        for (size_t k = 0; k < 4; k++) {
            uint8_t w = (uint8_t)(want >> (8 * k));
            uint8_t g = (uint8_t)(have >> (8 * k));
            if (w != g) {
                note_flip(r, off + i + k, w, g);
            }
        }
    }
}

size_t psram_dma_flip_text(psram_dma_flip_t const *f, char *out, size_t cap) {
    int n = snprintf(out, cap, "+%u (%u mod 1024) want %02x got %02x xor %02x", (unsigned)f->off, (unsigned)(f->off % 1024),
                     f->want, f->got, (unsigned)(f->want ^ f->got));
    return n < 0 ? 0 : (size_t)n;
}

size_t psram_dma_hist_text(psram_dma_report_t const *r, char *out, size_t cap) {
    size_t n = 0;
    for (int b = 0; b < 8 && n < cap; b++) {
        int k = snprintf(out + n, cap - n, "%s%d:%u", b == 0 ? "bits " : " ", b, (unsigned)r->bit_hist[b]);
        n    += k < 0 ? 0 : (size_t)k;
    }
    if (n < cap) {
        int k = snprintf(out + n, cap - n, ", %u of %u at the same offset mod 1024", (unsigned)r->same_mod1k,
                         (unsigned)r->bad_bytes);
        n    += k < 0 ? 0 : (size_t)k;
    }
    return n < cap ? n : cap - 1;
}
