#include "psram_scan.h"

#include "psram_pattern.h"
#include "psram_quar.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK       PSRAM_QUAR_GRAIN
#define MAX_CHUNKS  1024
#define WIN_LO      0x48400000u // the suspect window of #40: bad bytes near 0x4855BA81, 4 MB check ended at 0x485C0A80
#define WIN_HI      0x48800000u
#define LINE        128
#define PSRAM_CAPS  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SPARSE_XOR  0xAAAAAAAAu // BadgeVMS' boot test: every 8th word of the PSRAM, address ^ this
#define MAX_REGIONS 6
#define PASSES      4           // 0 sparse, 1 dense, 2 dense inverted, 3 dense again

static char const TAG[] = "psram";

typedef struct {
    uint8_t *mem;
    size_t   bad[PASSES];
} chunk_t;

typedef struct {
    char const *name;
    uintptr_t   addr;
    size_t      len;
} region_t;

static chunk_t       s_chunks[MAX_CHUNKS]; // every chunk grabbed, sorted by address
static size_t        s_n;
static chunk_t       s_held[MAX_CHUNKS];   // kept for good (quarantine)
static size_t        s_nheld;
static psram_quar_t  s_quar;
static region_t      s_regions[MAX_REGIONS];
static size_t        s_nregions;
static struct {
    size_t   scanned, bad_chunks, pass_bad[PASSES], bad_bytes;
    uint32_t xor_or, up_or, down_or;
    int64_t  us;
    uintptr_t lowest, highest;
    size_t   gaps_in_window;
} s_stat;

static int by_addr(void const *a, void const *b) {
    uintptr_t x = (uintptr_t)((chunk_t const *)a)->mem, y = (uintptr_t)((chunk_t const *)b)->mem;
    return x < y ? -1 : x > y;
}

static void sync(uint8_t *mem, size_t len) {
    esp_cache_msync(mem, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

// BadgeVMS' pattern on one chunk: every 8th word = its address ^ 0xAAAAAAAA. Returns bad words; records bits/extent.
static size_t sparse_pass(uint8_t *mem, psram_check_t *acc) {
    uint32_t *w = (uint32_t *)mem;
    size_t    n = CHUNK / 4;
    for (size_t i = 0; i < n; i += 8) {
        w[i] = (uint32_t)(uintptr_t)&w[i] ^ SPARSE_XOR;
    }
    sync(mem, CHUNK);
    size_t bad = 0;
    for (size_t i = 0; i < n; i += 8) {
        uint32_t want = (uint32_t)(uintptr_t)&w[i] ^ SPARSE_XOR;
        uint32_t diff = w[i] ^ want;
        if (!diff) {
            continue;
        }
        if (bad++ == 0 && !acc->bad_bytes) {
            acc->first_bad = i * 4;
        }
        acc->bad_bytes++;
        acc->last_bad = i * 4 + 3;
        acc->xor_or  |= diff;
        acc->up_or   |= w[i] & ~want;
        acc->down_or |= want & ~w[i];
    }
    return bad;
}

static void dense_pass(uint8_t *mem, uint32_t mask, psram_check_t *out) {
    psram_pattern_fill_x(mem, CHUNK, (uintptr_t)mem, mask);
    sync(mem, CHUNK);
    *out = psram_pattern_check_x(mem, CHUNK, (uintptr_t)mem, mask);
}

// Folds one pass result into the chunk's extent and the global bit statistics
static void fold(chunk_t *c, int pass, psram_check_t const *r, size_t *first, size_t *last) {
    if (!r->bad_bytes) {
        return;
    }
    c->bad[pass] = r->bad_bytes;
    *first = r->first_bad < *first ? r->first_bad : *first;
    *last  = r->last_bad > *last ? r->last_bad : *last;
    s_stat.pass_bad[pass] += r->bad_bytes;
    s_stat.xor_or  |= r->xor_or;
    s_stat.up_or   |= r->up_or;
    s_stat.down_or |= r->down_or;
}

static void scan_chunk(chunk_t *c) {
    psram_check_t r = {0};
    size_t        first = SIZE_MAX, last = 0;
    memset(c->bad, 0, sizeof(c->bad));
    sparse_pass(c->mem, &r);
    fold(c, 0, &r, &first, &last);
    static uint32_t const masks[3] = {0, 0xFFFFFFFFu, 0};
    for (int p = 0; p < 3; p++) {
        dense_pass(c->mem, masks[p], &r);
        fold(c, p + 1, &r, &first, &last);
    }
    s_stat.scanned++;
    size_t total = c->bad[0] + c->bad[1] + c->bad[2] + c->bad[3];
    if (total) {
        s_stat.bad_chunks++;
        s_stat.bad_bytes += total;
        uint32_t bits = s_stat.xor_or;
        psram_quar_note(&s_quar, (uintptr_t)c->mem + first, (uintptr_t)c->mem + last, (uint32_t)total, bits);
        ESP_LOGW(TAG, "scan: chunk %p bad: sparse %u, dense %u / %u / %u bytes, extent +%u..+%u", (void *)c->mem,
                 (unsigned)c->bad[0], (unsigned)c->bad[1], (unsigned)c->bad[2], (unsigned)c->bad[3], (unsigned)first,
                 (unsigned)last);
    }
}

static void grab_all(void) {
    while (s_n < MAX_CHUNKS) {
        uint8_t *m = heap_caps_aligned_alloc(LINE, CHUNK, PSRAM_CAPS);
        if (!m) {
            break;
        }
        s_chunks[s_n++].mem = m;
    }
    qsort(s_chunks, s_n, sizeof(chunk_t), by_addr);
}

static bool in_window(chunk_t const *c) {
    uintptr_t a = (uintptr_t)c->mem;
    return a + CHUNK > WIN_LO && a < WIN_HI;
}

// Gaps between neighbouring chunks inside the window are memory somebody else already owns (code, rodata, early heap)
static void log_map(void) {
    extern int _instruction_reserved_start, _instruction_reserved_end, _rodata_reserved_start, _rodata_reserved_end;
    s_stat.lowest  = s_n ? (uintptr_t)s_chunks[0].mem : 0;
    s_stat.highest = s_n ? (uintptr_t)s_chunks[s_n - 1].mem + CHUNK : 0;
    ESP_LOGW(TAG, "map: %u chunks of 64 KB grabbed from %p to %p, %u KB free PSRAM left; text %p..%p, rodata %p..%p",
             (unsigned)s_n, (void *)s_stat.lowest, (void *)s_stat.highest,
             (unsigned)(heap_caps_get_free_size(PSRAM_CAPS) >> 10), (void *)&_instruction_reserved_start,
             (void *)&_instruction_reserved_end, (void *)&_rodata_reserved_start, (void *)&_rodata_reserved_end);
    for (size_t i = 1; i < s_n; i++) {
        uintptr_t end = (uintptr_t)s_chunks[i - 1].mem + CHUNK, next = (uintptr_t)s_chunks[i].mem;
        if (next - end > 4096) {
            if (next > WIN_LO && end < WIN_HI) {
                s_stat.gaps_in_window++;
            }
            ESP_LOGW(TAG, "map: in use %p..%p (%u KB)%s", (void *)end, (void *)next, (unsigned)((next - end) >> 10),
                     next > WIN_LO && end < WIN_HI ? " in the scan window" : "");
        }
    }
}

static void keep_or_free(void) {
    for (size_t i = 0; i < s_n; i++) {
        if (psram_quar_overlaps(&s_quar, (uintptr_t)s_chunks[i].mem, CHUNK)) {
            s_held[s_nheld++] = s_chunks[i];
        } else {
            heap_caps_free(s_chunks[i].mem);
        }
    }
}

// Chunks well outside the window go back at once, so the rest of the boot is not starved while the window is scanned
static void drop_far(void) {
    size_t kept = 0;
    for (size_t i = 0; i < s_n; i++) {
        uintptr_t a = (uintptr_t)s_chunks[i].mem;
        if (a + 4 * CHUNK > WIN_LO && a < WIN_HI + 4 * CHUNK) {
            s_chunks[kept++] = s_chunks[i];
        } else {
            heap_caps_free(s_chunks[i].mem);
        }
    }
    s_n = kept;
}

void psram_scan_run(void) {
    int64_t start = esp_timer_get_time();
    size_t  free0 = heap_caps_get_free_size(PSRAM_CAPS);
    grab_all();
    log_map();
    drop_far();
    for (size_t i = 0; i < s_n; i++) {
        if (in_window(&s_chunks[i])) {
            scan_chunk(&s_chunks[i]);
        }
    }
    keep_or_free();
    s_stat.us = esp_timer_get_time() - start;
    ESP_LOGW(TAG, "scan: window %p..%p, %u chunks scanned in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u; "
                  "flipped bits %08x (0->1 %08x, 1->0 %08x); quarantine %u range(s), %u KB held of %u KB, free PSRAM %u -> %u KB",
             (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)(s_stat.us / 1000), (unsigned)s_stat.bad_chunks,
             (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1], (unsigned)s_stat.pass_bad[2],
             (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or, (unsigned)s_stat.up_or, (unsigned)s_stat.down_or,
             (unsigned)s_quar.n, (unsigned)(s_nheld * CHUNK >> 10), (unsigned)(psram_quar_bytes(&s_quar) >> 10),
             (unsigned)(free0 >> 10), (unsigned)(heap_caps_get_free_size(PSRAM_CAPS) >> 10));
}

void psram_scan_note_region(char const *name, void const *addr, size_t len) {
    if (s_nregions < MAX_REGIONS) {
        s_regions[s_nregions++] = (region_t){name, (uintptr_t)addr, len};
    }
}

void psram_scan_print(void) {
    console_reply("psram scan: window %p..%p, %u chunks in %u ms, %u bad chunks, bad bytes sparse %u dense %u/%u/%u, "
                  "bits %08x (0->1 %08x, 1->0 %08x), %u gaps (memory in use) in the window",
                  (void *)WIN_LO, (void *)WIN_HI, (unsigned)s_stat.scanned, (unsigned)(s_stat.us / 1000),
                  (unsigned)s_stat.bad_chunks, (unsigned)s_stat.pass_bad[0], (unsigned)s_stat.pass_bad[1],
                  (unsigned)s_stat.pass_bad[2], (unsigned)s_stat.pass_bad[3], (unsigned)s_stat.xor_or,
                  (unsigned)s_stat.up_or, (unsigned)s_stat.down_or, (unsigned)s_stat.gaps_in_window);
    if (!s_quar.n) {
        console_reply("psram quarantine: none (no bad bytes in the window)");
    }
    for (size_t i = 0; i < s_quar.n; i++) {
        psram_quar_range_t const *r = &s_quar.r[i];
        console_reply("psram quarantine %u: %p..%p (%u KB), bad bytes %p..%p (%u seen), bits %08x", (unsigned)i,
                      (void *)r->lo, (void *)r->hi, (unsigned)((r->hi - r->lo) >> 10), (void *)r->first_bad,
                      (void *)r->last_bad, (unsigned)r->bad_bytes, (unsigned)r->bits);
    }
    console_reply("psram quarantine: %u KB in ranges, %u KB held in %u chunks", (unsigned)(psram_quar_bytes(&s_quar) >> 10),
                  (unsigned)(s_nheld * CHUNK >> 10), (unsigned)s_nheld);
    for (size_t i = 0; i < s_nregions; i++) {
        region_t const *g = &s_regions[i];
        console_reply("psram region %s %p..%p%s", g->name, (void *)g->addr, (void *)(g->addr + g->len),
                      psram_quar_overlaps(&s_quar, g->addr, g->len) ? " OVERLAPS the quarantine" : "");
    }
}

void psram_scan_watch(unsigned ms) {
    if (!s_nheld) {
        console_reply("psram qwatch: nothing held");
        return;
    }
    for (size_t i = 0; i < s_nheld; i++) {
        psram_pattern_fill_x(s_held[i].mem, CHUNK, (uintptr_t)s_held[i].mem, 0);
        sync(s_held[i].mem, CHUNK);
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
    size_t total = 0;
    for (size_t i = 0; i < s_nheld; i++) {
        sync(s_held[i].mem, CHUNK);
        psram_check_t r = psram_pattern_check_x(s_held[i].mem, CHUNK, (uintptr_t)s_held[i].mem, 0);
        if (r.bad_bytes) {
            total += r.bad_bytes;
            console_reply("psram qwatch: chunk %p %u bad bytes +%u..+%u, bits %08x (0->1 %08x, 1->0 %08x)", (void *)s_held[i].mem,
                          (unsigned)r.bad_bytes, (unsigned)r.first_bad, (unsigned)r.last_bad, (unsigned)r.xor_or,
                          (unsigned)r.up_or, (unsigned)r.down_or);
        }
    }
    console_reply("psram qwatch: %u chunks (%u KB) written once, read back after %u ms: %u bad bytes", (unsigned)s_nheld,
                  (unsigned)(s_nheld * CHUNK >> 10), ms, (unsigned)total);
}
