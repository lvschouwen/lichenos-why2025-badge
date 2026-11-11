#pragma once

#include <stdbool.h>
#include <stdint.h>

// "fw psram dma [rounds] [burst]" (#121): the DMA-path gate for any PSRAM clock. An AXI-GDMA channel (esp_async_memcpy)
// copies pattern buffers PSRAM -> internal RAM, internal -> PSRAM and PSRAM -> PSRAM; every copy is compared with the
// pattern (psram_dma.c) and damage is listed as offset:xor, the offset mod 1024 and a bit histogram. The CPU canaries
// and the boot check of psram_guard never saw the bit 3 flips of #120 because they only use the cache path.
// Runs on the console task; needs 3 x 1 MB of PSRAM and 8 KB of internal DMA RAM, all freed afterwards.
// "fw psram dma sweep [sector]" runs the write/read legs and both SD legs over every 1 MB of the largest free PSRAM block.
void psram_dma_cmd(char const *args);
// One quick round (all patterns, the SD legs when a card is mounted) for a caller that must tell a pass from a round
// that never ran (the Bench, the stress run, #278); a failed round at 120 MHz is PSRAM evidence (#353). BUSY = another
// DMA gate (a console run, another round) holds it, NOT_RUN = no memory or no AXI-GDMA. info (may be NULL)
// gets the judged bad bytes (AXI-GDMA legs and the bounced SD leg), the DMA errors and whether the SD legs were skipped;
// zeros unless the round ran.
typedef enum { PSRAM_DMA_ROUND_PASS, PSRAM_DMA_ROUND_FAIL, PSRAM_DMA_ROUND_BUSY, PSRAM_DMA_ROUND_NOT_RUN } psram_dma_round_t;
typedef struct {
    uint32_t bad_bytes;
    unsigned errors;
    bool     sd_skipped;
} psram_dma_round_info_t;
psram_dma_round_t psram_dma_quick_round(psram_dma_round_info_t *info);

// The SD leg (#120 flipped on the SDMMC -> PSRAM path; the AXI-GDMA legs alone did not reproduce it at 120 MHz):
// sdcard.c registers a read-only raw sector reader at start. psram_guard cannot depend on sdcard (board -> psram_guard
// -> sdcard -> board would be a cycle). begin/end wrap the card's batch mutex; read moves 512-byte sectors by the
// SDMMC controller's own DMA into dst (DMA-capable, 128-byte aligned, a multiple of 512 bytes). read_bounced reads the
// same sectors the way the FatFs driver does since #121 E4: into an internal bounce buffer, then a CPU copy into dst.
// Nothing is written.
typedef struct {
    bool (*begin)(uint32_t wait_ms);
    void (*end)(bool failed);
    bool (*read)(void *dst, uint32_t sector, uint32_t count);
    bool (*read_bounced)(void *dst, uint32_t sector, uint32_t count);
    bool (*mounted)(void);
} psram_dma_sd_t;
void psram_dma_set_sd(psram_dma_sd_t const *sd);
// True while the SD card is mounted ("fw psram dl" refuses then); false when sdcard.c never registered
bool psram_dma_sd_mounted(void);
