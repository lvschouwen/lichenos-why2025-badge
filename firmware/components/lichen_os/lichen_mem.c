// lichen_alloc (#181): heap_caps with the placement rule and the per-app count (mem_book.c) on top. The count is under
// a spinlock: an allocation can come from any task, and the book's work is a few compares. The book is in internal RAM
// so a critical section never touches PSRAM (whose cache can be off during a flash write). The owner is the caller's
// context (#230 E1b), checked before anything is booked: lock-free, no LVGL lock.
#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "gate_book.h"

#include "dma_watch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"

#include <string.h>

_Static_assert(MEM_BOOK_OWNERS >= LICHEN_MAX_APPS + 1, "a row for every app and the OS");

static char const TAG[] = "mem";

static mem_book_t   book; // ~1 KB internal .bss
static portMUX_TYPE book_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         went_unbooked; // the owner table was full once: frees without a row are expected from then on

static void book_take(void) {
    portENTER_CRITICAL(&book_mux);
}
static void book_give(void) {
    portEXIT_CRITICAL(&book_mux);
}

bool gate_os_resolve(uint32_t ctx, char const **id, char const **name, char const *what) {
    lichen_app_t const *app = NULL;
    if (!lichen_sys_ctx_resolve(ctx, &app, what)) {
        return false;
    }
    *id   = app ? app->id : NULL;
    *name = app ? app->name : NULL;
    return true;
}

static void block_of(void const *ptr, bool *internal, uint32_t *bytes) {
    *internal = !esp_ptr_external_ram(ptr);
    *bytes    = (uint32_t)heap_caps_get_allocated_size((void *)ptr);
}

static gate_env_t const env = {.resolve = gate_os_resolve, .os = lichen_sys_ctx_os, .enter = lichen_ctx_enter,
                               .take = book_take, .give = book_give, .block = block_of};

// The first allocation the book had no row for (logged once); later ones are expected
static bool first_unbooked(void) {
    portENTER_CRITICAL(&book_mux);
    bool first    = !went_unbooked;
    went_unbooked = true;
    portEXIT_CRITICAL(&book_mux);
    return first;
}

// align 0: the heap's default
static void *alloc_aligned(lichen_ctx_t ctx, size_t align, size_t size, uint32_t flags) {
    bool internal = (flags & LICHEN_MEM_INTERNAL) != 0;
    if (internal == ((flags & LICHEN_MEM_BULK) != 0) || size == 0 || size > UINT32_MAX) {
        ESP_LOGE(TAG, "ctx 0x%08lx: lichen_alloc of %u B with flags 0x%x refused: BULK or INTERNAL, one of them",
                 (unsigned long)ctx, (unsigned)size, (unsigned)flags);
        return NULL;
    }
    gate_owner_t who;
    mem_charge_t charge = gate_mem_charge(&env, &book, ctx, internal, (uint32_t)size, &who);
    if (charge == MEM_GATE_REFUSED) {
        return NULL; // logged and counted by the context table
    }
    char const *owner = who.label ? who.label : MEM_BOOK_OS;
    if (charge == MEM_CHARGE_QUOTA) {
        ESP_LOGW(TAG, "%s: %u B of internal RAM refused: over the app's %u B", owner, (unsigned)size,
                 MEM_BOOK_INTERNAL_QUOTA);
        return NULL;
    }
    if (charge == MEM_CHARGE_NO_ROW && first_unbooked()) {
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
            mem_book_release(&book, ctx, internal, (uint32_t)size);
        } else if (got > size) {
            mem_book_adjust(&book, ctx, internal, (uint32_t)(got - size)); // the free gives back what the heap says
        }
        portEXIT_CRITICAL(&book_mux);
    }
    return p;
}

void *lichen_alloc(lichen_ctx_t ctx, size_t size, uint32_t flags) {
    return alloc_aligned(ctx, 0, size, flags);
}

void *lichen_alloc_aligned(lichen_ctx_t ctx, size_t align, size_t size, uint32_t flags) {
    return align ? alloc_aligned(ctx, align, size, flags) : NULL; // 0 is not an alignment: lichen_alloc is the plain one
}

void lichen_free(lichen_ctx_t ctx, void *ptr) {
    if (!ptr) {
        return;
    }
    char const   *label = NULL;
    uint32_t      bytes = 0;
    mem_release_t rel   = gate_mem_release(&env, &book, ctx, ptr, &label, &bytes);
    if (rel == MEM_GATE_RELEASE_REFUSED) {
        return; // refused (logged, counted) before ptr was looked at: the block, if it is one, stays allocated
    }
    char const *owner = label ? label : MEM_BOOK_OS;
    portENTER_CRITICAL(&book_mux);
    bool unbooked = went_unbooked;
    portEXIT_CRITICAL(&book_mux);
    if (rel == MEM_RELEASE_OVER || (rel == MEM_RELEASE_NO_ROW && !unbooked)) {
        // Freed under another owner than it was allocated with, or never from lichen_alloc: some count is now wrong
        ESP_LOGW(TAG, "%s: lichen_free of %p (%u B) is more than it holds: freed under the wrong owner?", owner, ptr,
                 (unsigned)bytes);
    }
    heap_caps_free(ptr);
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
    dma_watch_report();
}

void lichen_mem_console_register(void) {
    console_set_mem_report(mem_report);
    dma_watch_start();
}
