#include "psram_dma_cmd.h"

#include "psram_dma.h"

#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_console.h"
#include "lichen_random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_BYTES     (1024u * 1024)   // each PSRAM buffer: 1 MB covers every 1 KB spot many times (#120: 859 mod 1024)
#define CHUNK_BYTES   (32u * 1024)     // the internal buffer: internal DMA RAM is scarce, 32 KB and freed afterwards
#define PIECE_BYTES   (256u * 1024)    // PSRAM -> PSRAM is moved in pieces: the driver builds a descriptor list per call
#define ALIGN         128              // the L2 cache line: DMA buffers never share a line with anything else
#define MAX_ROUNDS    1000
#define DMA_WAIT_MS   2000
#define FLAGS_FLUSH   (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)
#define SD_SECTOR     512
#define SD_PIECE      4096             // FatFs reads 4 KB sectors (CONFIG_FATFS_SECTOR_4096): the transfer shape of #120
#define SD_WAIT_MS    2000

typedef struct {
    async_memcpy_handle_t mcp;
    SemaphoreHandle_t     done;
    uint8_t              *src;   // PSRAM, filled by the CPU
    uint8_t              *dst;   // PSRAM, written by DMA from internal RAM
    uint8_t              *dst2;  // PSRAM, written by DMA from src
    uint8_t              *in;    // internal DMA RAM
    psram_dma_pattern_t   pat;
    uint32_t              seed;
    unsigned              errors; // DMA calls refused or timed out, cache syncs refused
    size_t                bad_total;
    int64_t               us[4];  // time spent per leg over the run, for MB/s
    uint32_t              sd_sector; // first card sector of the SD leg
    bool                  sd_skipped;
} run_t;

static char const *const legs[4] = {"psram>int", "int>psram", "psram>psram", "sd>psram"};
static psram_dma_sd_t    s_sd;

void psram_dma_set_sd(psram_dma_sd_t const *sd) {
    s_sd = *sd;
}

static bool IRAM_ATTR on_done(async_memcpy_handle_t mcp, async_memcpy_event_t *ev, void *ctx) {
    (void)mcp;
    (void)ev;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(((run_t *)ctx)->done, &woken);
    return woken == pdTRUE;
}

static void sync_mem(run_t *r, void *p, size_t len, int flags) {
    if (esp_cache_msync(p, len, flags) != ESP_OK) {
        r->errors++;
    }
}

// One DMA copy, waited for. Returns false when the driver refused or the transfer never finished.
static bool copy(run_t *r, void *dst, void const *src, size_t len) {
    if (esp_async_memcpy(r->mcp, dst, (void *)src, len, on_done, r) != ESP_OK ||
        xSemaphoreTake(r->done, pdMS_TO_TICKS(DMA_WAIT_MS)) != pdTRUE) {
        r->errors++;
        return false;
    }
    return true;
}

static void show(run_t *r, int leg, psram_dma_report_t const *rep) {
    char text[112];
    psram_dma_hist_text(rep, text, sizeof(text));
    console_reply("psram dma %s %s seed %08x: %u bad bytes, %s", legs[leg], psram_dma_pattern_name(r->pat), (unsigned)r->seed,
                  (unsigned)rep->bad_bytes, text);
    for (size_t i = 0; i < rep->kept; i++) {
        psram_dma_flip_text(&rep->flips[i], text, sizeof(text));
        console_reply("psram dma   %s", text);
    }
    r->bad_total += rep->bad_bytes;
}

// Leg 0: DMA reads PSRAM into the internal buffer, chunk by chunk; the CPU checks each chunk
static void leg_psram_to_int(run_t *r) {
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < BIG_BYTES; off += CHUNK_BYTES) {
        sync_mem(r, r->in, CHUNK_BYTES, FLAGS_FLUSH); // no stale lines of the internal buffer in the L1 cache
        if (!copy(r, r->in, r->src + off, CHUNK_BYTES)) {
            break;
        }
        sync_mem(r, r->in, CHUNK_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        psram_dma_compare(r->in, CHUNK_BYTES, r->pat, r->seed, off, &rep);
    }
    r->us[0] += esp_timer_get_time() - t0;
    if (rep.bad_bytes) {
        show(r, 0, &rep);
    }
}

// Leg 1: the CPU fills the internal buffer, DMA writes it into PSRAM; the whole 1 MB is checked afterwards
static void leg_int_to_psram(run_t *r) {
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    sync_mem(r, r->dst, BIG_BYTES, FLAGS_FLUSH);
    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < BIG_BYTES; off += CHUNK_BYTES) {
        psram_dma_fill(r->in, CHUNK_BYTES, r->pat, r->seed, off);
        sync_mem(r, r->in, CHUNK_BYTES, FLAGS_FLUSH);
        if (!copy(r, r->dst + off, r->in, CHUNK_BYTES)) {
            break;
        }
    }
    r->us[1] += esp_timer_get_time() - t0;
    sync_mem(r, r->dst, BIG_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    psram_dma_compare(r->dst, BIG_BYTES, r->pat, r->seed, 0, &rep);
    if (rep.bad_bytes) {
        show(r, 1, &rep);
    }
}

// Leg 2: DMA copies PSRAM to PSRAM (both lanes of the bus busy, as the display and SD paths do)
static void leg_psram_to_psram(run_t *r) {
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    sync_mem(r, r->dst2, BIG_BYTES, FLAGS_FLUSH);
    int64_t t0 = esp_timer_get_time();
    for (size_t off = 0; off < BIG_BYTES; off += PIECE_BYTES) {
        if (!copy(r, r->dst2 + off, r->src + off, PIECE_BYTES)) {
            break;
        }
    }
    r->us[2] += esp_timer_get_time() - t0;
    sync_mem(r, r->dst2, BIG_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    psram_dma_compare(r->dst2, BIG_BYTES, r->pat, r->seed, 0, &rep);
    if (rep.bad_bytes) {
        show(r, 2, &rep);
    }
}

// The source as the CPU reads it back through the cache: tells a damaged copy from a damaged source
static void check_src(run_t *r) {
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    psram_dma_compare(r->src, BIG_BYTES, r->pat, r->seed, 0, &rep);
    if (rep.bad_bytes) {
        char text[112];
        psram_dma_hist_text(&rep, text, sizeof(text));
        console_reply("psram dma source (CPU path) %s: %u bad bytes, %s", psram_dma_pattern_name(r->pat), (unsigned)rep.bad_bytes, text);
        r->bad_total += rep.bad_bytes;
    }
}

// Leg 3: the SDMMC controller's DMA reads 1 MB of the card into PSRAM in 4 KB pieces, and the same sectors into
// internal RAM as the reference (a fixed-spot flip would show in two PSRAM reads alike). Read-only.
static void leg_sd_to_psram(run_t *r) {
    if (!s_sd.read || !s_sd.begin(SD_WAIT_MS)) {
        r->sd_skipped = true;
        return;
    }
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    bool    failed = false;
    int64_t t0     = esp_timer_get_time();
    for (size_t off = 0; off < BIG_BYTES && !failed; off += SD_PIECE) {
        uint32_t sector = r->sd_sector + (uint32_t)(off / SD_SECTOR);
        sync_mem(r, r->in, SD_PIECE, FLAGS_FLUSH);
        sync_mem(r, r->dst + off, SD_PIECE, FLAGS_FLUSH);
        failed = !s_sd.read(r->in, sector, SD_PIECE / SD_SECTOR) || !s_sd.read(r->dst + off, sector, SD_PIECE / SD_SECTOR);
        if (!failed) {
            sync_mem(r, r->in, SD_PIECE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            sync_mem(r, r->dst + off, SD_PIECE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            psram_dma_compare_buf(r->dst + off, r->in, SD_PIECE, off, &rep);
        }
    }
    r->us[3] += esp_timer_get_time() - t0;
    s_sd.end(failed);
    if (failed) {
        r->errors++;
        console_reply("psram dma %s: card read failed", legs[3]);
    }
    if (rep.bad_bytes) {
        r->pat = PSRAM_DMA_PATTERNS; // "?" in the report: the card's bytes, no pattern
        show(r, 3, &rep);
    }
}

static void one_pattern(run_t *r, psram_dma_pattern_t pat) {
    r->pat  = pat;
    r->seed = lichen_random_u32();
    psram_dma_fill(r->src, BIG_BYTES, pat, r->seed, 0);
    sync_mem(r, r->src, BIG_BYTES, FLAGS_FLUSH); // the pattern is in the RAM, not only in the cache
    leg_psram_to_int(r);
    leg_int_to_psram(r);
    leg_psram_to_psram(r);
    check_src(r);
}

static bool setup(run_t *r, unsigned burst) {
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.backlog               = 4;
    cfg.dma_burst_size        = burst;
    r->done                   = xSemaphoreCreateBinary();
    r->src                    = heap_caps_aligned_alloc(ALIGN, BIG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->dst                    = heap_caps_aligned_alloc(ALIGN, BIG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->dst2                   = heap_caps_aligned_alloc(ALIGN, BIG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->in                     = heap_caps_aligned_alloc(ALIGN, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!r->done || !r->src || !r->dst || !r->dst2 || !r->in) {
        console_reply("psram dma: out of memory (3 x 1 MB PSRAM, 32 KB internal DMA RAM)");
        return false;
    }
    esp_err_t err = esp_async_memcpy_install_gdma_axi(&cfg, &r->mcp);
    if (err != ESP_OK) {
        console_reply("psram dma: AXI-GDMA memcpy not available: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void teardown(run_t *r) {
    if (r->mcp) {
        esp_async_memcpy_uninstall(r->mcp);
    }
    heap_caps_free(r->in);
    heap_caps_free(r->dst2);
    heap_caps_free(r->dst);
    heap_caps_free(r->src);
    if (r->done) {
        vSemaphoreDelete(r->done);
    }
}

static unsigned mbps(int64_t us, unsigned rounds, unsigned mb_per_round) {
    return us > 0 ? (unsigned)((uint64_t)rounds * mb_per_round * BIG_BYTES / (uint64_t)us) : 0; // MB/s = bytes/us
}

void psram_dma_cmd(char const *args) {
    unsigned rounds = 1, burst = 64, sector = 0;
    if (sscanf(args, "%u %u %u", &rounds, &burst, &sector) < 1) {
        rounds = 1;
    }
    if (rounds < 1 || rounds > MAX_ROUNDS || (burst != 16 && burst != 32 && burst != 64 && burst != 128)) {
        console_reply("psram dma [rounds 1..%d] [burst 16|32|64|128] [first card sector of the SD leg]", MAX_ROUNDS);
        return;
    }
    static run_t r; // the ISR callback points here; one run at a time (the console runs commands one by one)
    memset(&r, 0, sizeof(r));
    r.sd_sector = sector;
    if (!setup(&r, burst)) {
        teardown(&r);
        return;
    }
    console_reply("psram dma: %u round(s) x %d patterns, 1 MB each, AXI-GDMA burst %u, legs %s %s %s, %s 1 MB from sector %u",
                  rounds, PSRAM_DMA_PATTERNS, burst, legs[0], legs[1], legs[2], legs[3], sector);
    int64_t t0 = esp_timer_get_time();
    for (unsigned n = 0; n < rounds && !r.errors; n++) {
        for (int p = 0; p < PSRAM_DMA_PATTERNS; p++) {
            one_pattern(&r, (psram_dma_pattern_t)p);
        }
        leg_sd_to_psram(&r);
        if (rounds > 1 && ((n + 1) % 10 == 0 || n + 1 == rounds)) {
            console_reply("psram dma: round %u/%u, %u bad bytes so far", n + 1, rounds, (unsigned)r.bad_total);
        }
    }
    console_reply("psram dma: %u bad bytes in %u round(s), %u errors, %s %u MB/s, %s %u MB/s, %s %u MB/s, %s %s, %u ms%s",
                  (unsigned)r.bad_total, rounds, r.errors, legs[0], mbps(r.us[0], rounds, PSRAM_DMA_PATTERNS), legs[1],
                  mbps(r.us[1], rounds, PSRAM_DMA_PATTERNS), legs[2], mbps(r.us[2], rounds, PSRAM_DMA_PATTERNS), legs[3],
                  r.sd_skipped ? "skipped (no card)" : "run", (unsigned)((esp_timer_get_time() - t0) / 1000),
                  r.errors ? " (stopped: DMA refused, timed out, a cache sync or a card read failed)" : "");
    teardown(&r);
}
