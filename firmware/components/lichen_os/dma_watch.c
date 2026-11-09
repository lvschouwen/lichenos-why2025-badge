// The DMA low-water watch (#256). heap_caps keeps only the lowest DMA free ever, not when or who. Two halves:
// - the heap's alloc hook (CONFIG_HEAP_USE_HOOKS) keeps the largest internal allocation since the last sample, with
//   the name of the task that made it, copied while that task is certainly alive (it is the one running). An
//   allocation in an ISR is blamed on the task it interrupted;
// - an esp_timer sample (no task of its own) reads the DMA min four times a second, takes the hook's slot and feeds
//   both to dma_low.c, which keeps the drops. A peak shorter than the period still shows (the mark is the heap's),
//   only its moment is rounded to the period.
// Internal RAM is the DMA-capable heap on the P4, so every internal allocation lowers it, not just MALLOC_CAP_DMA.
#include "dma_watch.h"

#include "dma_low.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"

#include <stdatomic.h>
#include <stddef.h>

#if !CONFIG_HEAP_USE_HOOKS
#error "the DMA low-water watch needs CONFIG_HEAP_USE_HOOKS (firmware/sdkconfig.defaults)"
#endif

static char const TAG[] = "mem";

#define DMA_WATCH_US    (250 * 1000)
#define DMA_BIG_MIN     1024 // an esp_hosted Wi-Fi TX frame copy is ~1.55 KB; 20 of them queue up (#256)

// FreeRTOS's own current-task table (tasks.c; global for the port's context switch). Read directly: the hook runs
// where flash may be off, and xTaskGetCurrentTaskHandle lives in flash. A task name sits where StaticTask_t's
// ucDummy7 does: FreeRTOS asserts that StaticTask_t mirrors its TCB.
extern StaticTask_t *volatile pxCurrentTCBs[];

// The hook's slot: DRAM (.bss), written by any core, possibly in an ISR or with the cache off. Lock-free: the size is
// claimed by a compare-and-swap; gen is bumped before the claim and after the name copy, so a reader that sees gen
// odd or changed knows the name may not belong to the size and says "?" rather than blame the wrong task.
static struct {
    _Atomic uint32_t size;
    _Atomic uint32_t gen;
    char             task[DMA_LOW_TASK_CAP];
} big;

// Every successful heap allocation passes here: two compares for nearly all of them (size, address); a new largest
// internal one adds a load, a CAS and a 16-byte copy. No calls, no locks, no flash, no heap. In IRAM through
// esp_heap_caps.h's declaration (HEAP_IRAM_ATTR; a second section attribute here would conflict with it).
void esp_heap_trace_alloc_hook(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    intptr_t a = (intptr_t)ptr;
    if (size < DMA_BIG_MIN || a < SOC_MEM_INTERNAL_LOW || a >= SOC_MEM_INTERNAL_HIGH) {
        return;
    }
    uint32_t want = size > UINT32_MAX ? UINT32_MAX : (uint32_t)size;
    uint32_t cur  = atomic_load_explicit(&big.size, memory_order_relaxed);
    if (want <= cur) {
        return;
    }
    atomic_fetch_add_explicit(&big.gen, 1, memory_order_acq_rel);
    if (atomic_compare_exchange_strong_explicit(&big.size, &cur, want, memory_order_acq_rel, memory_order_relaxed)) {
        StaticTask_t *tcb = pxCurrentTCBs[esp_cpu_get_core_id()];
        for (int i = 0; i < DMA_LOW_TASK_CAP; i++) {
            big.task[i] = tcb ? (char)tcb->ucDummy7[i] : '\0'; // NUL-padded by FreeRTOS; the reader terminates anyway
        }
    }
    atomic_fetch_add_explicit(&big.gen, 1, memory_order_release);
}

// Closes the hook's window: its largest allocation's size, and the name of who made it ("?" if a hook was writing)
static uint32_t take_big(char task[DMA_LOW_TASK_CAP]) {
    uint32_t g1 = atomic_load_explicit(&big.gen, memory_order_acquire);
    for (int i = 0; i < DMA_LOW_TASK_CAP; i++) {
        task[i] = big.task[i];
    }
    task[DMA_LOW_TASK_CAP - 1] = '\0';
    uint32_t size              = atomic_exchange_explicit(&big.size, 0, memory_order_acq_rel);
    uint32_t g2                = atomic_load_explicit(&big.gen, memory_order_acquire);
    if ((g1 & 1) || g1 != g2) {
        task[0] = '?';
        task[1] = '\0';
    }
    return size;
}

static dma_low_t    dma_low; // ~250 B internal .bss under dma_low_mux (internal: a critical section never touches PSRAM)
static portMUX_TYPE dma_low_mux = portMUX_INITIALIZER_UNLOCKED;

static void dma_watch(void *arg) {
    (void)arg;
    static uint32_t seen = UINT32_MAX; // the timer task only
    // The window closes before the mark is read: an allocation in between counts towards the next one
    char     task[DMA_LOW_TASK_CAP];
    uint32_t taken = take_big(task);
    uint32_t min   = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    if (min == seen) {
        return; // the usual tick: the mark only ever moves down
    }
    seen              = min;
    uint32_t free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint64_t at_ms    = (uint64_t)esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&dma_low_mux);
    bool kept = dma_low_feed(&dma_low, min, free_now, at_ms, taken, taken ? task : NULL);
    portEXIT_CRITICAL(&dma_low_mux);
    if (kept) {
        ESP_LOGI(TAG, "dma min %u (free %u), largest internal allocation since the last look %u B by %s", (unsigned)min,
                 (unsigned)free_now, (unsigned)taken, taken ? task : "-");
    }
}

void dma_watch_start(void) {
    static esp_timer_handle_t timer;
    if (timer) {
        return;
    }
    esp_timer_create_args_t const args = {.callback = dma_watch, .name = "dma_low"};
    if (esp_timer_create(&args, &timer) != ESP_OK) {
        timer = NULL;
        ESP_LOGW(TAG, "no DMA low-water watch: fw mem shows no dma_low lines");
        return;
    }
    dma_watch(NULL); // the first sample before the timer can fire: what boot took so far
    if (esp_timer_start_periodic(timer, DMA_WATCH_US) != ESP_OK) {
        ESP_LOGW(TAG, "DMA low-water watch not started: fw mem shows the boot figure only");
    }
}

void dma_watch_report(void) {
    dma_low_t snap; // ~250 B onto the console's stack: no PSRAM inside the critical section
    portENTER_CRITICAL(&dma_low_mux);
    snap = dma_low;
    portEXIT_CRITICAL(&dma_low_mux);
    char line[160];
    for (unsigned i = 0; dma_low_line(&snap, i, line, sizeof(line)); i++) {
        console_reply("%s", line);
    }
}
