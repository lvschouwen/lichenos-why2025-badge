#pragma once

#include <stdbool.h>
#include <stdint.h>

// "fw psram dma [rounds] [burst]" (#121): the DMA-path gate for any PSRAM clock. An AXI-GDMA channel (esp_async_memcpy)
// copies pattern buffers PSRAM -> internal RAM, internal -> PSRAM and PSRAM -> PSRAM; every copy is compared with the
// pattern (psram_dma.c) and damage is listed as offset:xor, the offset mod 1024 and a bit histogram. The CPU canaries
// and the boot check of psram_guard never saw the bit 3 flips of #120 because they only use the cache path.
// Runs on the console task; needs 3 x 1 MB of PSRAM and 32 KB of internal DMA RAM, all freed afterwards.
void psram_dma_cmd(char const *args);

// The SD leg (#120 flipped on the SDMMC -> PSRAM path; the AXI-GDMA legs alone did not reproduce it at 120 MHz):
// sdcard.c registers a read-only raw sector reader at start. psram_guard cannot depend on sdcard (board -> psram_guard
// -> sdcard -> board would be a cycle). begin/end wrap the card's batch mutex; read moves 512-byte sectors by the
// SDMMC controller's own DMA into dst (DMA-capable, 128-byte aligned, a multiple of 512 bytes). Nothing is written.
typedef struct {
    bool (*begin)(uint32_t wait_ms);
    void (*end)(bool failed);
    bool (*read)(void *dst, uint32_t sector, uint32_t count);
} psram_dma_sd_t;
void psram_dma_set_sd(psram_dma_sd_t const *sd);
