#include "psram_sweep.h"

#include "psram_learn.h"
#include "psram_scan.h"
#include "psram_tuning.h"

#include "esp_attr.h"
#include "esp_ipc_isr.h"
#include "esp_log.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/cache_ll.h"
#include "hal/mspi_ll.h"
#include "soc/iomux_mspi_pin_reg.h"
#include "soc/iomux_mspi_pin_struct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Why this is built the way it is: code, rodata and the display framebuffer live in PSRAM. While a setting that may not
// work is applied, nothing may touch PSRAM except our own test pages. So the step runs from IRAM with every callee inline
// (or in IRAM), interrupts off, the other core parked in the IDF's IRAM stall, and the whole cache written back first (a
// dirty line of somebody else evicted meanwhile would be written with the bad setting). Data it touches is in internal
// RAM. The original setting is restored, and the test pages dropped from the cache, before the step returns.

#define MAX_BAD    24
#define MAX_GOOD   2
#define PAGE       4096u
#define NSET       PSRAM_DELAYLINES
#define NPHASE     PSRAM_PHASES

static char const TAG[] = "psram";

typedef struct {
    uint8_t phase, dqs, data;
} setting_t;

typedef struct {
    uint8_t *bad[MAX_BAD];
    uint8_t *good[MAX_GOOD];
    unsigned nbad, ngood, passes;
} pages_t;

typedef struct {
    uint32_t bad_plain, bad_inv, good_bytes;
} result_t;

static pages_t  s_pages;
static result_t s_res[NPHASE][NSET];

static inline __attribute__((always_inline)) void apply(setting_t s) {
    mspi_timing_ll_set_dqs_phase(MSPI_LL_DQS_ID_0, (mspi_ll_dqs_phase_t)s.phase);
    mspi_timing_ll_set_dqs_phase(MSPI_LL_DQS_ID_1, (mspi_ll_dqs_phase_t)s.phase);
    for (int i = 0; i < MSPI_LL_PIN_MAX; i++) {
        mspi_timing_ll_set_delayline((mspi_ll_pin_t)i, (i == MSPI_LL_PIN_DQS0 || i == MSPI_LL_PIN_DQS1) ? s.dqs : s.data);
    }
}

static inline __attribute__((always_inline)) uint32_t word_at(uintptr_t addr) {
    uint32_t x = (uint32_t)addr * 0x9E3779B1u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x;
}

static inline __attribute__((always_inline)) uint32_t bytes_differ(uint32_t d) {
    return ((d & 0xffu) != 0) + ((d & 0xff00u) != 0) + ((d & 0xff0000u) != 0) + ((d & 0xff000000u) != 0);
}

// One page: pattern in, to the RAM, cache dropped, back out; returns the differing bytes
static inline __attribute__((always_inline)) uint32_t page_roundtrip(uint8_t *page, uint32_t mask) {
    volatile uint32_t *w = (volatile uint32_t *)page;
    for (unsigned i = 0; i < PAGE / 4; i++) {
        w[i] = word_at((uintptr_t)&w[i]) ^ mask;
    }
    cache_ll_writeback_addr(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, (uint32_t)page, PAGE);
    cache_ll_invalidate_addr(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, (uint32_t)page, PAGE);
    uint32_t bad = 0;
    for (unsigned i = 0; i < PAGE / 4; i++) {
        bad += bytes_differ(w[i] ^ (word_at((uintptr_t)&w[i]) ^ mask));
    }
    return bad;
}

static void IRAM_ATTR __attribute__((noinline)) step(setting_t try_s, setting_t orig, result_t *out) {
    pages_t const *p = &s_pages;
    result_t       r = {0};
    esp_ipc_isr_stall_other_cpu();
    uint32_t prev;
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(prev));
    cache_ll_writeback_all(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL);
    apply(try_s);
    for (unsigned pass = 0; pass < p->passes; pass++) {
        for (unsigned i = 0; i < p->nbad; i++) {
            r.bad_plain += page_roundtrip(p->bad[i], 0);
            r.bad_inv += page_roundtrip(p->bad[i], 0xFFFFFFFFu);
        }
        for (unsigned i = 0; i < p->ngood; i++) {
            r.good_bytes += page_roundtrip(p->good[i], 0) + page_roundtrip(p->good[i], 0xFFFFFFFFu);
        }
    }
    apply(orig);
    (void)*(volatile uint32_t *)IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG; // the writes have landed
    for (unsigned i = 0; i < p->nbad; i++) { // anything read under the trial setting is dropped
        cache_ll_invalidate_addr(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, (uint32_t)p->bad[i], PAGE);
    }
    for (unsigned i = 0; i < p->ngood; i++) {
        cache_ll_invalidate_addr(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, (uint32_t)p->good[i], PAGE);
    }
    if (prev & 8u) {
        __asm__ volatile("csrsi mstatus, 8");
    }
    esp_ipc_isr_release_other_cpu();
    *out = r;
}

static setting_t current(void) {
    setting_t s;
    s.phase = (uint8_t)REG_GET_FIELD(IOMUX_MSPI_PIN_PSRAM_DQS_0_PIN0_REG, IOMUX_MSPI_PIN_REG_PSRAM_DQS_0_PHASE);
    s.dqs   = (uint8_t)MSPI_IOMUX.psram_pin_group.dqs0.reg_psram_dqs_delay_90;
    s.data  = (uint8_t)MSPI_IOMUX.psram_pin_group.pin_group0[0].reg_psram_pin_dlc;
    return s;
}

static void print_row(char const *label, unsigned ph, bool good) {
    char line[NSET * 6 + 32];
    int  n = snprintf(line, sizeof(line), "%s", label);
    for (unsigned i = 0; i < NSET; i++) {
        result_t const *r = &s_res[ph][i];
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u", (unsigned)(good ? r->good_bytes : r->bad_plain + r->bad_inv));
    }
    console_reply("%s", line);
}

static void sweep_phase(unsigned ph, setting_t orig) {
    for (unsigned i = 0; i < NSET; i++) {
        setting_t t = {.phase = (uint8_t)ph};
        psram_tuning_delays((uint8_t)i, &t.data, &t.dqs);
        step(t, orig, &s_res[ph][i]);
        vTaskDelay(1);
    }
    char label[40];
    snprintf(label, sizeof(label), "psram sweep ph%u bad-pages:", ph);
    print_row(label, ph, false);
    ESP_LOGW(TAG, "sweep ph%u done", ph);
}

void psram_sweep_cmd(char const *args) {
    unsigned passes = (unsigned)atoi(args);
    int      only   = -1;
    char const *sp  = strchr(args[0] ? args : " ", ' ');
    if (sp && sp[1]) {
        only = atoi(sp + 1);
    }
    passes = passes ? passes : 3;
    s_pages.passes = passes > 16 ? 16 : passes;
    size_t ng;
    s_pages.nbad  = (unsigned)psram_scan_test_pages(s_pages.bad, MAX_BAD, s_pages.good, MAX_GOOD, &ng);
    s_pages.ngood = (unsigned)ng;
    if (!s_pages.nbad) {
        console_reply("psram sweep: no held known-bad pages to test with");
        return;
    }
    setting_t orig = current();
    console_reply("psram sweep WARNING: the display blanks / flickers for the whole run (interrupts are off while the PSRAM timing is changed)");
    console_reply("psram sweep: %u known-bad + %u good pages, %u passes x (plain, inverted); original ph%u dqs %u data %u. "
                  "Rows: idx 0..30 = dqs 15..1, 0, data 1..15; numbers = bad bytes",
                  s_pages.nbad, s_pages.ngood, s_pages.passes, orig.phase, orig.dqs, orig.data);
    for (unsigned k = 0; k < NPHASE; k++) {
        unsigned ph = (orig.phase + k) % NPHASE; // the running phase first
        if (only >= 0 && ph != (unsigned)only) {
            continue;
        }
        sweep_phase(ph, orig);
    }
    for (unsigned ph = 0; ph < NPHASE; ph++) {
        if (only >= 0 && ph != (unsigned)only) {
            continue;
        }
        char label[40];
        snprintf(label, sizeof(label), "psram sweep ph%u good-pages:", ph);
        print_row(label, ph, true);
    }
    setting_t now = current();
    console_reply("psram sweep: setting after the sweep ph%u dqs %u data %u (%s)", now.phase, now.dqs, now.data,
                  now.phase == orig.phase && now.dqs == orig.dqs && now.data == orig.data ? "restored" : "DIFFERS");
}
