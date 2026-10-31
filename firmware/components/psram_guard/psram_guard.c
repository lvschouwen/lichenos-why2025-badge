#include "psram_guard.h"

#include "psram_boot.h"
#include "psram_learned.h"
#include "psram_pattern.h"
#include "psram_scan.h"
#include "psram_sweep.h"

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG_BLOCKS    4
#define BIG_BYTES     (64 * 1024)
#define SMALL_BYTES   (4 * 1024)
#define MAX_BLOCKS    (BIG_BLOCKS + 3) // + heap_caps small, + LVGL small, + update mode
#define PERIOD_MS     100
#define SHOW_BYTES    16
#define PSRAM_CAPS    (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    uint8_t    *mem; // NULL = slot unused; cache-line aligned, inside raw
    void       *raw; // what to free
    size_t      len;
    char const *kind;
    bool        from_lvgl;
    uint32_t    checks;
    uint32_t    hits;
    uint32_t    bad_total; // damaged bytes over all hits
} block_t;

static char const TAG[] = "psram";

static block_t           blocks[MAX_BLOCKS];
static SemaphoreHandle_t lock;
static uint32_t          passes;
static int64_t           last_pass_us;
static int64_t           max_pass_us;
static uint32_t          last_hit_block = UINT32_MAX;
static uintptr_t         last_hit_addr;
static size_t            last_hit_off;
static uint32_t          last_hit_bad;
static int64_t           last_hit_ms;

// Whole cache lines, so the write-back/invalidate of one canary never touches a neighbour (and the other way round)
static void *aligned_in(void *raw) {
    return (void *)(((uintptr_t)raw + PSRAM_LINE - 1) & ~(uintptr_t)(PSRAM_LINE - 1));
}

// Fill and push the pattern out to PSRAM, dropping the cache lines, so the next read really comes from the RAM
static void fill_block(uint8_t *mem, size_t len) {
    psram_pattern_fill(mem, len, (uintptr_t)mem);
    esp_cache_msync(mem, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static psram_check_t check_block(uint8_t *mem, size_t len) {
    esp_cache_msync(mem, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    return psram_pattern_check(mem, len, (uintptr_t)mem);
}

static int add_block(size_t len, char const *kind, bool lvgl) {
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (blocks[i].mem) {
            continue;
        }
        size_t   size = len + PSRAM_LINE; // len is a multiple of the line size
        void    *raw  = lvgl ? lv_malloc(size) : heap_caps_malloc(size, PSRAM_CAPS);
        if (!raw) {
            ESP_LOGW(TAG, "no PSRAM for the %s canary", kind);
            return -1;
        }
        uint8_t *mem = aligned_in(raw);
        fill_block(mem, len);
        blocks[i] = (block_t){.mem = mem, .raw = raw, .len = len, .kind = kind, .from_lvgl = lvgl};
        return i;
    }
    return -1;
}

static void hex(uint8_t const *p, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        snprintf(out + 2 * i, 3, "%02x", p[i]);
    }
}

static void report(int id, block_t *b, psram_check_t const *res) {
    uint8_t *at = b->mem + res->first_bad;
    size_t   n  = b->len - res->first_bad < SHOW_BYTES ? b->len - res->first_bad : SHOW_BYTES;
    uint8_t  want[SHOW_BYTES];
    uint8_t  got[SHOW_BYTES];
    for (size_t i = 0; i < n; i++) {
        want[i] = psram_pattern_byte((uintptr_t)b->mem + res->first_bad + i);
    }
    memcpy(got, at, n); // read once more before it is refilled: what the memory holds now
    char want_hex[2 * SHOW_BYTES + 1];
    char got_hex[2 * SHOW_BYTES + 1];
    hex(want, n, want_hex);
    hex(got, n, got_hex);
    int64_t ms = esp_timer_get_time() / 1000;
    ESP_LOGW(TAG, "canary %d (%s) damaged at %p (+%u of %u), %u bad bytes, expected %s found %s, uptime %" PRId64 " ms",
             id, b->kind, (void *)at, (unsigned)res->first_bad, (unsigned)b->len, (unsigned)res->bad_bytes, want_hex,
             got_hex, ms);
    ESP_LOGW(TAG, "  canary %d: %u single-bit words, %u bad lines (%u whole), xor of first bad word %08x", id,
             (unsigned)res->bit_flips, (unsigned)res->bad_lines, (unsigned)res->full_lines, (unsigned)res->first_xor);
    psram_learned_mark_buf(b->mem, b->len, (uintptr_t)b->mem, 0, res->first_bad, res->last_bad);
    psram_learned_feed((uintptr_t)b->mem + res->first_bad, (uintptr_t)b->mem + res->last_bad, "canary");
    b->hits++;
    b->bad_total      += (uint32_t)res->bad_bytes;
    last_hit_block     = (uint32_t)id;
    last_hit_addr      = (uintptr_t)at;
    last_hit_off       = res->first_bad;
    last_hit_bad       = (uint32_t)res->bad_bytes;
    last_hit_ms        = ms;
}

static void verify_all(void) {
    int64_t start = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < MAX_BLOCKS; i++) {
        block_t *b = &blocks[i];
        if (!b->mem) {
            continue;
        }
        psram_check_t res = check_block(b->mem, b->len);
        b->checks++;
        if (res.bad_bytes) {
            report(i, b, &res);
            fill_block(b->mem, b->len);
        }
    }
    passes++;
    last_pass_us = esp_timer_get_time() - start;
    if (last_pass_us > max_pass_us) {
        max_pass_us = last_pass_us;
    }
    xSemaphoreGive(lock);
}

static void guard_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000)); // NVS is up by now: restart-log note and retry policy of a bad boot
    psram_boot_policy();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
        verify_all();
    }
}

// "psram test <KB>": one fill / write-back / invalidate / read-back round trip over a fresh block, on demand
static void psram_test(char const *kb_text) {
    unsigned kb = (unsigned)atoi(kb_text);
    if (kb == 0 || kb > 4096) {
        console_reply("psram test <KB> (1..4096)");
        return;
    }
    if (strstr(kb_text, "dense")) {
        psram_boot_dense_test(kb);
        return;
    }
    size_t   len = (size_t)kb * 1024;
    uint8_t *mem = heap_caps_aligned_alloc(PSRAM_LINE, len, PSRAM_CAPS);
    if (!mem) {
        console_reply("psram test: no PSRAM for %u KB", kb);
        return;
    }
    int64_t       start = esp_timer_get_time();
    fill_block(mem, len);
    psram_check_t res = check_block(mem, len);
    if (res.bad_bytes) {
        psram_learned_mark_buf(mem, len, (uintptr_t)mem, 0, res.first_bad, res.last_bad);
        psram_learned_feed((uintptr_t)mem + res.first_bad, (uintptr_t)mem + res.last_bad, "fw psram test");
    }
    console_reply("psram test %u KB at %p: %u bad bytes, %u single-bit words, %u bad lines (%u whole), first bad +%u xor %08x, %u us",
                  kb, (void *)mem, (unsigned)res.bad_bytes, (unsigned)res.bit_flips, (unsigned)res.bad_lines,
                  (unsigned)res.full_lines, (unsigned)res.first_bad, (unsigned)res.first_xor,
                  (unsigned)(esp_timer_get_time() - start));
    heap_caps_free(mem);
}

static void cmd_psram(char const *args) {
    if (strncmp(args, "qwatch", 6) == 0) {
        unsigned ms = (unsigned)atoi(args + 6);
        psram_scan_watch(ms ? ms : 2000);
        return;
    }
    if (strncmp(args, "sweep", 5) == 0) {
        psram_sweep_cmd(args + 5 + (args[5] == ' '));
        return;
    }
    if (strncmp(args, "forget", 6) == 0) {
        psram_learned_forget();
        return;
    }
    if (strncmp(args, "test", 4) == 0) {
        psram_test(args + 4);
        return;
    }
    psram_boot_print();
    psram_scan_print();
    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t hits = 0;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        block_t const *b = &blocks[i];
        if (!b->mem) {
            continue;
        }
        hits += b->hits;
        console_reply("psram canary %d %s %p %u bytes: %u checks, %u hits, %u bad bytes", i, b->kind, (void *)b->mem,
                      (unsigned)b->len, (unsigned)b->checks, (unsigned)b->hits, (unsigned)b->bad_total);
    }
    console_reply("psram %u passes every %d ms, last %u us, longest %u us, %u hits in the canaries now held", (unsigned)passes,
                  PERIOD_MS, (unsigned)last_pass_us, (unsigned)max_pass_us, (unsigned)hits);
    if (last_hit_block == UINT32_MAX) {
        console_reply("psram no damage seen");
    } else {
        console_reply("psram last hit: canary %u at %p (+%u), %u bad bytes, %u s ago", (unsigned)last_hit_block,
                      (void *)last_hit_addr, (unsigned)last_hit_off, (unsigned)last_hit_bad,
                      (unsigned)((esp_timer_get_time() / 1000 - last_hit_ms) / 1000));
    }
    xSemaphoreGive(lock);
}

// The big blocks go in between short-lived spacers, so they sit far apart in the heap instead of side by side
static void add_spread_blocks(void) {
    void *spacers[BIG_BLOCKS - 1] = {0};
    for (int i = 0; i < BIG_BLOCKS; i++) {
        add_block(BIG_BYTES, "64K", false);
        if (i < BIG_BLOCKS - 1) {
            spacers[i] = heap_caps_malloc((size_t)(1 << i) * 1024 * 1024, PSRAM_CAPS);
        }
    }
    for (int i = 0; i < BIG_BLOCKS - 1; i++) {
        heap_caps_free(spacers[i]);
    }
}

void psram_guard_start(void) {
    lock = xSemaphoreCreateMutex();
    if (!lock) {
        return;
    }
    psram_scan_run(); // before anything else takes memory: map the suspect window, quarantine bad chunks (#86)
    psram_boot_check(); // then, on an untouched heap, before the canaries and the rest of the boot load the bus
    heap_caps_monitor_local_minimum_free_size_start(); // "mem psram" min: from here, not the scan that held all of it
    add_spread_blocks();
    add_block(SMALL_BYTES, "4K", false);
    add_block(SMALL_BYTES, "4K-lvgl", true);
    console_register("psram", "psram [test <KB> [dense] | qwatch <ms> | sweep [passes [phase]] | forget] (learned bad range, quarantine, canary blocks in PSRAM: checks, damage seen, last hit; or a round-trip test)", cmd_psram);
    if (xTaskCreatePinnedToCore(guard_task, "psram_guard", 4096, NULL, 1, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "canary task failed to start");
    }
}

void psram_guard_update_begin(void) {
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool have = false;
    for (int i = 0; i < MAX_BLOCKS; i++) {
        have = have || (blocks[i].mem && strcmp(blocks[i].kind, "update") == 0);
    }
    if (!have) {
        add_block(BIG_BYTES, "update", false);
    }
    xSemaphoreGive(lock);
}

void psram_guard_update_end(void) {
    if (!lock) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < MAX_BLOCKS; i++) {
        if (blocks[i].mem && strcmp(blocks[i].kind, "update") == 0) {
            if (blocks[i].from_lvgl) {
                lv_free(blocks[i].raw);
            } else {
                heap_caps_free(blocks[i].raw);
            }
            blocks[i] = (block_t){0};
        }
    }
    xSemaphoreGive(lock);
}
