#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The pure half of "fw psram dma" (#121): test patterns for DMA copies PSRAM <-> internal RAM and the compare that
// turns a damaged copy into offset:xor lines, the offset mod 1024 (the SD->PSRAM flips of #120 sat at a fixed place
// in every 1 KB) and a per-bit histogram. Every byte is a function of (pattern, seed, offset), so any chunk of the
// logical buffer can be generated or checked on its own; the ESP side (psram_dma_cmd.c) moves the bytes.
// Host-tested in host_tests/psram_dma_test.c.

typedef enum {
    PSRAM_DMA_ADDR,     // the canary pattern of psram_pattern.c (address-dependent, every byte value)
    PSRAM_DMA_ADDR_INV, // its inverse: every bit seen as 0 and as 1 at every place
    PSRAM_DMA_WALK1,    // walking ones: byte at offset o is 1 << (o % 8)
    PSRAM_DMA_WALK0,    // walking zeros
    PSRAM_DMA_RANDOM,   // hash of (seed, word index): random-looking, reproducible from the seed
    PSRAM_DMA_JOURNAL,  // random, but the bytes at 859 mod 1024 cycle 0x11 0x19 0x17 0x1f: the bug of #120 in place
    PSRAM_DMA_PATTERNS
} psram_dma_pattern_t;

char const *psram_dma_pattern_name(psram_dma_pattern_t p);

// The 4 bytes at logical offset off (a multiple of 4), little-endian, so byte k of the word is the byte at off + k
uint32_t psram_dma_word(psram_dma_pattern_t p, uint32_t seed, size_t off);

// Fill buf (4-byte aligned, len a multiple of 4) with the bytes at logical offsets off .. off + len - 1
void psram_dma_fill(void *buf, size_t len, psram_dma_pattern_t p, uint32_t seed, size_t off);

typedef struct {
    size_t  off;  // logical offset of the damaged byte
    uint8_t want; // the pattern byte
    uint8_t got;  // what came out of the copy
} psram_dma_flip_t;

#define PSRAM_DMA_KEEP 8 // flips listed in detail; the rest is only counted

typedef struct {
    size_t           bad_bytes;    // bytes that differ, in total
    size_t           bit_hist[8];  // bad_bytes whose difference has bit b set
    size_t           same_mod1k;   // bad bytes at the same offset mod 1024 as the first one (a fixed spot, as in #120)
    size_t           kept;         // flips[] entries filled
    psram_dma_flip_t flips[PSRAM_DMA_KEEP];
} psram_dma_report_t;

void psram_dma_report_reset(psram_dma_report_t *r);

// Compare got (the copy, 4-byte aligned, len a multiple of 4) with the bytes expected at logical offsets off ..;
// accumulates into r so one report can cover a buffer checked chunk by chunk
void psram_dma_compare(void const *got, size_t len, psram_dma_pattern_t p, uint32_t seed, size_t off, psram_dma_report_t *r);

// Compare got with a reference copy ref (both 4-byte aligned, len a multiple of 4) read from somewhere else: for data
// whose expected value is not a pattern (the SD leg reads the card into PSRAM and into internal RAM). Accumulates.
void psram_dma_compare_buf(void const *got, void const *ref, size_t len, size_t off, psram_dma_report_t *r);

// "+4955 (859 mod 1024) want 11 got 19 xor 08"; returns the length (snprintf style)
size_t psram_dma_flip_text(psram_dma_flip_t const *f, char *out, size_t cap);

// "bits 0:0 1:0 2:0 3:5 4:0 5:0 6:0 7:0, 5 of 5 at the same offset mod 1024"; returns the length
// "fw psram dma sweep": how many 1 MB windows (win bytes) to take from the largest free block while leaving `reserve`
// bytes of the free PSRAM to the rest of the system for the whole run; 0 when fewer than min_windows fit
size_t psram_dma_sweep_windows(size_t largest, size_t free_total, size_t reserve, size_t win, size_t min_windows);

size_t psram_dma_hist_text(psram_dma_report_t const *r, char *out, size_t cap);

// PSRAM evidence for the speed switch (#353): bad bytes on the AXI-GDMA legs or the bounced SD leg (bad_total minus the
// direct SD leg's). A failed card read, a DMA timeout or a refused transfer fails the gate but proves nothing about PSRAM
bool psram_dma_evidence(size_t bad_total, size_t sd_bad_direct);
