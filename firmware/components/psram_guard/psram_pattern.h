#pragma once

#include <stddef.h>
#include <stdint.h>

// The address-dependent fill of the PSRAM canaries (#40). Every byte is a function of its logical address (tag + offset),
// so a block that was copied, shifted or overwritten by a neighbour's data does not look intact. Pure, host-tested in
// host_tests/psram_pattern_test.c; psram_guard.c uses it on real blocks (tag = the block's address).

// The expected byte at logical address addr
uint8_t psram_pattern_byte(uintptr_t addr);

// Fill len bytes of buf with the pattern for logical addresses tag .. tag + len - 1
void psram_pattern_fill(void *buf, size_t len, uintptr_t tag);

#define PSRAM_LINE 128 // the P4's cache line: damage in whole lines points at the cache/DMA path, single bits at the RAM

typedef struct {
    size_t   first_bad;  // offset of the first byte that differs (meaningful when bad_bytes > 0)
    size_t   bad_bytes;  // how many bytes differ
    size_t   bit_flips;  // 32-bit words that differ in exactly one bit
    size_t   bad_lines;  // 128-byte lines (by logical address) with at least one differing word
    size_t   full_lines; // lines whose every word differs
    size_t   last_bad;   // offset of the last byte that differs
    uint32_t xor_or;     // OR of every differing byte's difference, at its lane of the word: which bits ever flipped
    uint32_t up_or;      // OR of the bits found 1 that should be 0 (0 -> 1 errors), at their lane of the word
    uint32_t down_or;    // OR of the bits found 0 that should be 1 (1 -> 0 errors)
    uint32_t first_xor;  // expected ^ found of the first differing word (the whole word, aligned down)
} psram_check_t;

// Compare buf with the pattern for tag
psram_check_t psram_pattern_check(void const *buf, size_t len, uintptr_t tag);

// The same with every word XORed with mask (0xFFFFFFFF = the inverted pattern): the boot check runs both, so every bit
// is written and read back as 0 and as 1 (#40)
void          psram_pattern_fill_x(void *buf, size_t len, uintptr_t tag, uint32_t mask);
psram_check_t psram_pattern_check_x(void const *buf, size_t len, uintptr_t tag, uint32_t mask);
