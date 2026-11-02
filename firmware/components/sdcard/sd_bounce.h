#pragma once
// SD transfers that must not be DMA'd by the SDMMC controller straight into or out of the caller's buffer (#121): at
// 120 MHz PSRAM only the SDMMC controller's DMA flipped bits (the AXI-GDMA and the CPU stayed clean), so a buffer in
// PSRAM, and one the SDMMC DMA cannot use as it is (not cache-line aligned), goes through an internal DMA bounce
// buffer instead, a few sectors at a time. Pure: sdcard.c does the card I/O, host_tests/sd_bounce_test.c the rules.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The internal DMA-capable bounce buffer, allocated at mount and freed at unmount. 16 KB = 32 sectors per card
// command; on a tight heap the mount halves it down to SD_BOUNCE_MIN_BYTES.
#ifndef SD_BOUNCE_BYTES
#define SD_BOUNCE_BYTES (16 * 1024)
#endif
#define SD_BOUNCE_MIN_BYTES 4096

// True when a transfer has to go through the bounce buffer: the buffer is in PSRAM, or the SDMMC DMA cannot use it
// directly (dma_aligned: DMA-capable internal RAM and the host's check_buffer_alignment passes for the whole transfer)
bool sd_bounce_needed(bool in_psram, bool dma_aligned);

// One card command's worth of a bounced transfer: `count` sectors from `sector`, at byte `offset` of the caller's
// buffer, `bytes` long
typedef struct {
    uint32_t sector;
    uint32_t count;
    size_t   offset;
    size_t   bytes;
} sd_bounce_chunk_t;

typedef struct {
    uint32_t sector;    // next sector
    uint32_t left;      // sectors still to move
    uint32_t per_chunk; // whole sectors that fit the bounce buffer
    size_t   offset;    // next byte in the caller's buffer
    size_t   sector_size;
} sd_bounce_iter_t;

// False when the bounce buffer cannot hold one sector (or a size is 0): then nothing may be moved
bool sd_bounce_start(sd_bounce_iter_t *it, uint32_t sector, uint32_t count, size_t sector_size, size_t bounce_bytes);
// The next chunk; false when the transfer is done
bool sd_bounce_next(sd_bounce_iter_t *it, sd_bounce_chunk_t *out);

#ifdef __cplusplus
}
#endif
