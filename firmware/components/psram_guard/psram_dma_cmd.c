#include "psram_speed.h"
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

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_BYTES     (1024u * 1024)   // each PSRAM buffer: 1 MB covers every 1 KB spot many times (#120: 859 mod 1024)
#define CHUNK_BYTES   (8u * 1024)      // the internal buffer: internal DMA RAM is scarce (#256: 32 KB here, taken on every 120 MHz
                                       // boot by the trial's gate, pulled the DMA low-water mark ~37 KB down). 8 KB still spans
                                       // more than one AXI-GDMA descriptor (4 KB each)
#define PIECE_BYTES   (256u * 1024)    // PSRAM -> PSRAM is moved in pieces: the driver builds a descriptor list per call
#define ALIGN         128              // the L2 cache line: DMA buffers never share a line with anything else
#define MAX_ROUNDS    1000
#define DMA_WAIT_MS   2000
#define FLAGS_FLUSH   (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE)
#define SD_SECTOR     512
#define SD_PIECE      (32u * 1024)     // one card transfer of 32 KB (64 sectors): FatFs reads a whole cluster at once when the
                                       // destination is sector-aligned, so the SDMMC IDMAC runs several 4 KB descriptors in one
                                       // go; every #120 flip sat beyond the first 4 KB of the file, none inside the first descriptor
#define SD_WAIT_MS    2000
#define SWEEP_RESERVE (8u * 1024 * 1024) // free PSRAM the sweep leaves to the rest of the badge for its ~40 s
#define SWEEP_MIN_WINDOWS 4u

_Static_assert(SD_PIECE % CHUNK_BYTES == 0, "the SD reference is read into the internal buffer in whole chunks");

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
    int64_t               us[5];  // time spent per leg over the run, for MB/s
    uint32_t              sd_sector; // first card sector of the SD leg
    bool                  sd_skipped;
    size_t                sd_bad[2]; // bad bytes of the direct and the bounced SD leg
} run_t;

static char const *const legs[5] = {"psram>int", "int>psram", "psram>psram", "sd>psram", "sd>bounce>psram"};
static psram_dma_sd_t    s_sd;
static atomic_flag       s_busy = ATOMIC_FLAG_INIT; // one DMA gate at a time: the console command or the trial's

void psram_dma_set_sd(psram_dma_sd_t const *sd) {
    s_sd = *sd;
}

bool psram_dma_sd_mounted(void) {
    return s_sd.mounted && s_sd.mounted();
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
    char  text[112];
    void *at = leg == 0 ? (void *)r->src : leg == 2 ? (void *)r->dst2 : (void *)r->dst;
    psram_dma_hist_text(rep, text, sizeof(text));
    console_reply("psram dma %s %s seed %08x at %p: %u bad bytes, %s", legs[leg], psram_dma_pattern_name(r->pat),
                  (unsigned)r->seed, at, (unsigned)rep->bad_bytes, text);
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

// Leg 3: the SDMMC controller's DMA reads 1 MB of the card into PSRAM in 32 KB transfers, and the same sectors into
// internal RAM as the reference (a fixed-spot flip would show in two PSRAM reads alike). Leg 4 (bounced): the same
// sectors through the SD driver's internal bounce buffer and a CPU copy, the path FatFs takes since #121 E4; same
// clock, same card, so a flip in leg 3 and none in leg 4 pins it on SDMMC DMA into PSRAM. Read-only.
static void leg_sd(run_t *r, bool bounced) {
    int  leg  = bounced ? 4 : 3;
    bool (*read)(void *, uint32_t, uint32_t) = bounced ? s_sd.read_bounced : s_sd.read;
    if (!read || !s_sd.begin(SD_WAIT_MS)) {
        r->sd_skipped = true;
        return;
    }
    psram_dma_report_t rep;
    psram_dma_report_reset(&rep);
    bool    failed = false;
    int64_t t0     = esp_timer_get_time();
    for (size_t off = 0; off < BIG_BYTES && !failed; off += SD_PIECE) {
        uint32_t sector = r->sd_sector + (uint32_t)(off / SD_SECTOR);
        sync_mem(r, r->dst + off, SD_PIECE, FLAGS_FLUSH);
        failed = !read(r->dst + off, sector, SD_PIECE / SD_SECTOR); // the path under test: one 32 KB card transfer
        if (!failed) {
            // DMA wrote the RAM behind the cache: drop the lines. The CPU copy sits in dirty lines: write them back
            // (an invalidate would throw the data away) and drop them, so the compare reads what reached the PSRAM
            sync_mem(r, r->dst + off, SD_PIECE, bounced ? FLAGS_FLUSH : ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        }
        // The reference: the same sectors into internal RAM, a chunk at a time (the internal buffer is smaller, #256)
        for (size_t part = 0; part < SD_PIECE && !failed; part += CHUNK_BYTES) {
            sync_mem(r, r->in, CHUNK_BYTES, FLAGS_FLUSH);
            failed = !s_sd.read(r->in, sector + (uint32_t)(part / SD_SECTOR), CHUNK_BYTES / SD_SECTOR);
            if (!failed) {
                sync_mem(r, r->in, CHUNK_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                psram_dma_compare_buf(r->dst + off + part, r->in, CHUNK_BYTES, off + part, &rep);
            }
        }
    }
    r->us[leg] += esp_timer_get_time() - t0;
    s_sd.end(failed);
    if (failed) {
        r->errors++;
        console_reply("psram dma %s: card read failed", legs[leg]);
    }
    r->sd_bad[bounced] += rep.bad_bytes;
    if (rep.bad_bytes) {
        r->pat = PSRAM_DMA_PATTERNS; // "?" in the report: the card's bytes, no pattern
        show(r, leg, &rep);
    }
}

// "run (direct 0, bounced 0 bad bytes)" or "skipped"
static char const *sd_text(run_t const *r, char *buf, size_t cap) {
    if (r->sd_skipped) {
        return "skipped (no card)";
    }
    snprintf(buf, cap, "run (direct %u, bounced %u bad bytes)", (unsigned)r->sd_bad[0], (unsigned)r->sd_bad[1]);
    return buf;
}

static void legs_sd(run_t *r) {
    leg_sd(r, false);
    if (!r->sd_skipped && !r->errors) {
        leg_sd(r, true);
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
        console_reply("psram dma: out of memory (3 x 1 MB PSRAM, %u KB internal DMA RAM)", (unsigned)(CHUNK_BYTES >> 10));
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

// "psram dma sweep [sector]": the same legs over every 1 MB window of the largest free PSRAM block, so an address-
// dependent fault (#120's two files flipped at different offsets mod 1024: a fixed place in the RAM, not in the file)
// is met wherever it sits. Per window: DMA write from internal RAM, DMA read back, CPU read-back; then both SD legs
// (direct, bounced) window by window, reading on through the card from sector.
static void sweep_windows(run_t *r, uint8_t *block, size_t windows) {
    for (int p = 0; p < PSRAM_DMA_PATTERNS; p++) {
        r->pat  = (psram_dma_pattern_t)p;
        r->seed = lichen_random_u32();
        for (size_t w = 0; w < windows && !r->errors; w++) {
            r->dst = r->src = block + w * BIG_BYTES;
            leg_int_to_psram(r);
            leg_psram_to_int(r);
            check_src(r);
        }
    }
    uint32_t first = r->sd_sector;
    for (size_t w = 0; w < windows && !r->errors && !r->sd_skipped; w++) {
        r->dst       = block + w * BIG_BYTES;
        r->sd_sector = first + (uint32_t)(w * (BIG_BYTES / SD_SECTOR));
        legs_sd(r);
    }
}

// The gate of #121 for the 120 MHz switch: any bad byte on the AXI-GDMA legs or the bounced SD leg (what the badge ships
// since E4) or a DMA error. The direct SD leg is the unbounced path nothing uses any more: it is reported, not judged.
static bool gate_failed(run_t const *r) {
    return r->bad_total > r->sd_bad[0] || r->errors > 0;
}

static void sweep(run_t *r, unsigned burst) {
    bool gate = false;
    size_t   largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t   free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t   windows = psram_dma_sweep_windows(largest, free_ps, SWEEP_RESERVE, BIG_BYTES, SWEEP_MIN_WINDOWS);
    uint8_t *block   = windows ? heap_caps_aligned_alloc(ALIGN, windows * BIG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.backlog               = 4;
    cfg.dma_burst_size        = burst;
    r->done                   = xSemaphoreCreateBinary();
    r->in                     = heap_caps_aligned_alloc(ALIGN, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!block || !r->done || !r->in || esp_async_memcpy_install_gdma_axi(&cfg, &r->mcp) != ESP_OK) {
        console_reply("psram dma sweep: no memory or no AXI-GDMA (largest free PSRAM block %u KB, %u KB free, keeps %u MB, needs %u MB "
                      "windows)",
                      (unsigned)(largest >> 10), (unsigned)(free_ps >> 10), (unsigned)(SWEEP_RESERVE >> 20), SWEEP_MIN_WINDOWS);
    } else {
        console_reply("psram dma sweep: %u MB at %p..%p, AXI-GDMA burst %u, SD leg from sector %u", (unsigned)windows, (void *)block,
                      (void *)(block + windows * BIG_BYTES), burst, (unsigned)r->sd_sector);
        int64_t t0 = esp_timer_get_time();
        sweep_windows(r, block, windows);
        char sd[64];
        console_reply("psram dma sweep: %u bad bytes over %u MB, %u errors, SD legs %s, %u ms", (unsigned)r->bad_total,
                      (unsigned)windows, r->errors, sd_text(r, sd, sizeof(sd)), (unsigned)((esp_timer_get_time() - t0) / 1000));
        gate = gate_failed(r);
    }
    r->src = r->dst = r->dst2 = NULL;
    teardown(r);
    heap_caps_free(block);
    psram_speed_gate_result(gate); // after the buffers are freed: a failed gate at 120 MHz restarts the badge
}

// One automatic round (#121): every pattern on the three AXI-GDMA legs and the SD legs (skipped without a card, not a
// failure). Runs on its caller's task, not the console; refuses to overlap a console run or another round (BUSY).
psram_dma_round_t psram_dma_quick_round(psram_dma_round_info_t *info) {
    static run_t r;
    if (atomic_flag_test_and_set(&s_busy)) {
        if (info) {
            *info = (psram_dma_round_info_t){0};
        }
        return PSRAM_DMA_ROUND_BUSY;
    }
    memset(&r, 0, sizeof(r));
    psram_dma_round_t result = PSRAM_DMA_ROUND_NOT_RUN;
    if (setup(&r, 64)) {
        for (int p = 0; p < PSRAM_DMA_PATTERNS && !r.errors; p++) {
            one_pattern(&r, (psram_dma_pattern_t)p);
        }
        legs_sd(&r);
        result = gate_failed(&r) ? PSRAM_DMA_ROUND_FAIL : PSRAM_DMA_ROUND_PASS;
    }
    if (info) {
        *info = (psram_dma_round_info_t){.bad_bytes  = (uint32_t)(r.bad_total - r.sd_bad[0]),
                                         .errors     = r.errors,
                                         .sd_skipped = r.sd_skipped};
    }
    teardown(&r);
    atomic_flag_clear(&s_busy);
    return result;
}

// The 120 MHz trial's gate: busy is a pass here, a console run's result reaches psram_speed_gate_result itself
bool psram_dma_quick_gate(void) {
    psram_dma_round_t result = psram_dma_quick_round(NULL);
    return result == PSRAM_DMA_ROUND_PASS || result == PSRAM_DMA_ROUND_BUSY;
}

static void psram_dma_cmd_locked(char const *args);

void psram_dma_cmd(char const *args) {
    if (atomic_flag_test_and_set(&s_busy)) {
        console_reply("psram dma: another DMA gate is running (the 120 MHz trial's), try again in a few seconds");
        return;
    }
    psram_dma_cmd_locked(args);
    atomic_flag_clear(&s_busy);
}

static void psram_dma_cmd_locked(char const *args) {
    unsigned rounds = 1, burst = 64, sector = 0;
    static run_t r; // the ISR callback points here; one run at a time (the console runs commands one by one)
    if (sscanf(args, " sweep %u", &sector) == 1 || strcmp(args, " sweep") == 0 || strcmp(args, "sweep") == 0) {
        memset(&r, 0, sizeof(r));
        r.sd_sector = sector;
        sweep(&r, burst);
        return;
    }
    if (sscanf(args, "%u %u %u", &rounds, &burst, &sector) < 1) {
        rounds = 1;
    }
    if (rounds < 1 || rounds > MAX_ROUNDS || (burst != 16 && burst != 32 && burst != 64 && burst != 128)) {
        console_reply("psram dma [rounds 1..%d] [burst 16|32|64|128] [first card sector of the SD leg] | dma sweep [sector]", MAX_ROUNDS);
        return;
    }
    memset(&r, 0, sizeof(r));
    r.sd_sector = sector;
    if (!setup(&r, burst)) {
        teardown(&r);
        return;
    }
    console_reply("psram dma: %u round(s) x %d patterns, 1 MB each, AXI-GDMA burst %u, legs %s %s %s, %s and %s 1 MB from sector %u",
                  rounds, PSRAM_DMA_PATTERNS, burst, legs[0], legs[1], legs[2], legs[3], legs[4], sector);
    int64_t t0 = esp_timer_get_time();
    for (unsigned n = 0; n < rounds && !r.errors; n++) {
        for (int p = 0; p < PSRAM_DMA_PATTERNS; p++) {
            one_pattern(&r, (psram_dma_pattern_t)p);
        }
        legs_sd(&r);
        if (rounds > 1 && ((n + 1) % 10 == 0 || n + 1 == rounds)) {
            console_reply("psram dma: round %u/%u, %u bad bytes so far", n + 1, rounds, (unsigned)r.bad_total);
        }
    }
    char sd[64];
    console_reply("psram dma: %u bad bytes in %u round(s), %u errors, %s %u MB/s, %s %u MB/s, %s %u MB/s, SD legs %s, %u ms%s",
                  (unsigned)r.bad_total, rounds, r.errors, legs[0], mbps(r.us[0], rounds, PSRAM_DMA_PATTERNS), legs[1],
                  mbps(r.us[1], rounds, PSRAM_DMA_PATTERNS), legs[2], mbps(r.us[2], rounds, PSRAM_DMA_PATTERNS),
                  sd_text(&r, sd, sizeof(sd)), (unsigned)((esp_timer_get_time() - t0) / 1000),
                  r.errors ? " (stopped: DMA refused, timed out, a cache sync or a card read failed)" : "");
    teardown(&r);
    psram_speed_gate_result(gate_failed(&r));
}
