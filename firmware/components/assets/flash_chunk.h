// Flash reads into PSRAM through a small internal buffer (#364). esp_flash_read on SPI1 can only fill internal RAM: for
// a PSRAM destination it mallocs an internal temp buffer as big as the read (up to 16 KB) after walking the internal
// heap, which sets the DMA low-water mark (#256). Reading through the caller's own small internal buffer (a stack
// array) instead costs no heap at all and is safe with any number of concurrent callers. Pure, host-tested
// (host_tests/flash_chunk_test.c).
#pragma once

#include <stddef.h>
#include <stdint.h>

// The bounce buffer the badge's callers put on their stack: half the code standard's 1 KB frame line, and few enough
// flash calls for a 2 MB read (the assets at boot: about 3600)
#define FLASH_CHUNK_BOUNCE 512

// Reads n bytes at off into buf (internal RAM); 0 = done, anything else = failed
typedef int (*flash_chunk_read_fn)(void *ctx, size_t off, uint8_t *buf, size_t n);

// Copy len bytes at off into dst, read through bounce (cap bytes) one chunk at a time. 0 = done, -1 = a read failed
// or there is no bounce buffer.
int flash_chunk_copy(flash_chunk_read_fn read, void *ctx, size_t off, uint8_t *dst, size_t len, uint8_t *bounce,
                     size_t cap);

// Whether the len bytes at off equal expect, read through bounce the same way: 1 = equal, 0 = they differ, -1 = a read
// failed or there is no bounce buffer. Stops at the first chunk that differs.
int flash_chunk_same(flash_chunk_read_fn read, void *ctx, size_t off, uint8_t const *expect, size_t len,
                     uint8_t *bounce, size_t cap);
