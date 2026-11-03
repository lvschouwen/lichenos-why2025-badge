// The screenshot's wire compression (#146): the 16-bit pixels as runs and literals, so a UI frame (mostly flat colour)
// crosses the slow Wi-Fi link in about a tenth of the bytes. Pure and host-tested. Each call encodes a self-contained
// piece (runs never span two calls), so the sender can compress slice by slice with a small buffer.
//
// Stream of packets, pixels are 2 bytes each, copied as they sit in memory:
//   0x00..0x7F       literal: (c + 1) pixels follow
//   0x80..0xFE       run: (c & 0x7F) + 1 copies of the next pixel (1..127)
//   0xFF, u16 le e   run: 128 + e copies of the next pixel
#pragma once

#include <stddef.h>
#include <stdint.h>

#define OTA_RLE_RUN_MAX 65663 // longest run of one packet

// Most bytes ota_rle_encode writes for npx pixels (all literals: one control byte per 128 pixels)
size_t ota_rle_bound(size_t npx);

// Encode npx pixels (2 bytes each) into out, which holds at least ota_rle_bound(npx) bytes. Returns the bytes written.
size_t ota_rle_encode(uint8_t const *in, size_t npx, uint8_t *out);

// Decode a whole stream into out (capacity npx pixels). Returns the pixels decoded, or SIZE_MAX when the stream is
// malformed or would overflow. Used by the host test; tools/badge.sh has the same decoder in python.
size_t ota_rle_decode(uint8_t const *in, size_t len, uint8_t *out, size_t npx);
