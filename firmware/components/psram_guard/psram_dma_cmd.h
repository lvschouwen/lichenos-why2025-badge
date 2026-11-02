#pragma once

// "fw psram dma [rounds] [burst]" (#121): the DMA-path gate for any PSRAM clock. An AXI-GDMA channel (esp_async_memcpy)
// copies pattern buffers PSRAM -> internal RAM, internal -> PSRAM and PSRAM -> PSRAM; every copy is compared with the
// pattern (psram_dma.c) and damage is listed as offset:xor, the offset mod 1024 and a bit histogram. The CPU canaries
// and the boot check of psram_guard never saw the bit 3 flips of #120 because they only use the cache path.
// Runs on the console task; needs 3 x 1 MB of PSRAM and 32 KB of internal DMA RAM, all freed afterwards.
void psram_dma_cmd(char const *args);
