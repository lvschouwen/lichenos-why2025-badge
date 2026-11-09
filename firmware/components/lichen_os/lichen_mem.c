// lichen_alloc (#181): heap_caps with the placement rule and the per-app count (mem_book.c) on top. The count is under
// a spinlock: an allocation can come from any task, and the book's work is a few compares. The book is in internal RAM
// so a critical section never touches PSRAM (whose cache can be off during a flash write).
#include "lichen_mem.h"

#include "dma_low.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

_Static_assert(MEM_BOOK_OWNERS >= LICHEN_MAX_APPS + 1, "a row for every app and the OS");

static char const TAG[] = "mem";

static mem_book_t   book; // ~1 KB internal .bss
static portMUX_TYPE book_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         went_unbooked; // the owner table was full once: frees without a row are expected from then on

// align 0: the heap's default
static void *alloc_aligned(lichen_app_t const *app, size_t align, size_t size, uint32_t flags) {
    char const *owner    = app ? app->id : MEM_BOOK_OS;
    bool        internal = (flags & LICHEN_MEM_INTERNAL) != 0;
    if (internal == ((flags & LICHEN_MEM_BULK) != 0) || size == 0 || size > UINT32_MAX) {
        ESP_LOGE(TAG, "%s: lichen_alloc of %u B with flags 0x%x refused: BULK or INTERNAL, one of them", owner,
                 (unsigned)size, (unsigned)flags);
        return NULL;
    }
    portENTER_CRITICAL(&book_mux);
    mem_charge_t charge = mem_book_charge(&book, owner, internal, (uint32_t)size);
    bool         first  = charge == MEM_CHARGE_NO_ROW && !went_unbooked;
    went_unbooked       = went_unbooked || charge == MEM_CHARGE_NO_ROW;
    portEXIT_CRITICAL(&book_mux);
    if (charge == MEM_CHARGE_QUOTA) {
        ESP_LOGW(TAG, "%s: %u B of internal RAM refused: over the app's %u B", owner, (unsigned)size,
                 MEM_BOOK_INTERNAL_QUOTA);
        return NULL;
    }
    if (first) {
        // Bookkeeping never fails an allocation: this owner's memory is not counted (logged once)
        ESP_LOGW(TAG, "%s: no row left in the memory book (%d): allocated, but not counted", owner, MEM_BOOK_OWNERS);
    }
    uint32_t caps = (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
    void    *p;
    if (align) {
        p = heap_caps_aligned_alloc(align, size, caps);
        if (p && (flags & LICHEN_MEM_ZERO)) {
            memset(p, 0, size);
        }
    } else {
        p = (flags & LICHEN_MEM_ZERO) ? heap_caps_calloc(1, size, caps) : heap_caps_malloc(size, caps);
    }
    size_t   got  = p ? heap_caps_get_allocated_size(p) : 0;
    if (charge == MEM_CHARGE_OK) {
        portENTER_CRITICAL(&book_mux);
        if (!p) {
            mem_book_release(&book, owner, internal, (uint32_t)size);
        } else if (got > size) {
            mem_book_adjust(&book, owner, internal, (uint32_t)(got - size)); // the free gives back what the heap says
        }
        portEXIT_CRITICAL(&book_mux);
    }
    return p;
}

void *lichen_alloc(lichen_app_t const *app, size_t size, uint32_t flags) {
    return alloc_aligned(app, 0, size, flags);
}

void *lichen_alloc_aligned(lichen_app_t const *app, size_t align, size_t size, uint32_t flags) {
    return align ? alloc_aligned(app, align, size, flags) : NULL; // 0 is not an alignment: lichen_alloc is the plain one
}

void lichen_free(lichen_app_t const *app, void *ptr) {
    if (!ptr) {
        return;
    }
    bool     internal = !esp_ptr_external_ram(ptr);
    uint32_t bytes    = (uint32_t)heap_caps_get_allocated_size(ptr);
    char const *owner = app ? app->id : MEM_BOOK_OS;
    portENTER_CRITICAL(&book_mux);
    mem_release_t rel      = mem_book_release(&book, owner, internal, bytes);
    bool          unbooked = went_unbooked;
    portEXIT_CRITICAL(&book_mux);
    if (rel == MEM_RELEASE_OVER || (rel == MEM_RELEASE_NO_ROW && !unbooked)) {
        // Freed under another owner than it was allocated with, or never from lichen_alloc: some count is now wrong
        ESP_LOGW(TAG, "%s: lichen_free of %p (%u B) is more than it holds: freed under the wrong owner?", owner, ptr,
                 (unsigned)bytes);
    }
    heap_caps_free(ptr);
}

// The DMA low-water watch (#256): heap_caps keeps only the lowest DMA free ever, not when. An esp_timer sample (no
// task of its own) reads it four times a second and records each real drop with the uptime and the task running on
// the other core: the timer task itself is all this core could name. A peak shorter than the period still shows
// (the mark is the heap's), only its moment is rounded to the period.
#define DMA_WATCH_US (250 * 1000)

static dma_low_t    dma_low; // ~200 B internal .bss under dma_low_mux (internal: a critical section never touches PSRAM)
static portMUX_TYPE dma_low_mux = portMUX_INITIALIZER_UNLOCKED;

static void other_core_task(char *out, size_t cap) {
    snprintf(out, cap, "-");
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(esp_cpu_get_core_id() == 0 ? 1 : 0);
    char const  *n = t ? pcTaskGetName(t) : NULL; // a name to go on, read without a lock: the task may be ending
    if (n) {
        snprintf(out, cap, "%.*s", (int)(cap - 1), n);
    }
#endif
}

static void dma_watch(void *arg) {
    (void)arg;
    static uint32_t seen = UINT32_MAX; // the timer task only
    uint32_t        min  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    if (min == seen) {
        return; // the usual tick: the mark only ever moves down
    }
    seen = min;
    char task[DMA_LOW_TASK_CAP];
    other_core_task(task, sizeof(task));
    uint32_t free_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
    uint64_t at_ms    = (uint64_t)esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&dma_low_mux);
    bool kept = dma_low_feed(&dma_low, min, free_now, at_ms, task);
    portEXIT_CRITICAL(&dma_low_mux);
    if (kept) {
        ESP_LOGI(TAG, "dma min %u (free %u), task on the other core %s", (unsigned)min, (unsigned)free_now, task);
    }
}

static void dma_watch_start(void) {
    static esp_timer_handle_t timer;
    esp_timer_create_args_t const args = {.callback = dma_watch, .name = "dma_low"};
    if (timer || esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, DMA_WATCH_US) != ESP_OK) {
        ESP_LOGW(TAG, "no DMA low-water watch: fw mem shows no dma_low lines");
        return;
    }
    dma_watch(NULL); // the first sample now: what boot took so far
}

static void dma_low_report(void) {
    dma_low_t snap; // ~200 B onto the console's stack: no PSRAM inside the critical section
    portENTER_CRITICAL(&dma_low_mux);
    snap = dma_low;
    portEXIT_CRITICAL(&dma_low_mux);
    char line[128];
    for (unsigned i = 0; dma_low_line(&snap, i, line, sizeof(line)); i++) {
        console_reply("%s", line);
    }
}

// The lines "fw mem" adds: what each owner holds through lichen_alloc now, its peak, and refusals
static void mem_report(void) {
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        mem_use_t row; // one row at a time onto the stack: a short critical section, and no PSRAM inside it
        portENTER_CRITICAL(&book_mux);
        row = book.row[i];
        portEXIT_CRITICAL(&book_mux);
        mem_use_t const *r = &row;
        if (r->owner[0]) {
            console_reply("mem app %s internal %u (peak %u, quota %u) psram %u (peak %u) blocks %u refused %u", r->owner,
                          (unsigned)r->internal, (unsigned)r->internal_peak, MEM_BOOK_INTERNAL_QUOTA,
                          (unsigned)r->bulk, (unsigned)r->bulk_peak, (unsigned)r->blocks, (unsigned)r->refused);
        }
    }
    dma_low_report();
}

void lichen_mem_console_register(void) {
    console_set_mem_report(mem_report);
    dma_watch_start();
}
