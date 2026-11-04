// lichen_alloc (#181): heap_caps with the placement rule and the per-app count (mem_book.c) on top. The count is under
// a spinlock: an allocation can come from any task, and the book's work is a few compares.
#include "lichen_mem.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"

_Static_assert(MEM_BOOK_OWNERS >= LICHEN_MAX_APPS + 1, "a row for every app and the OS");

static char const TAG[] = "mem";

EXT_RAM_BSS_ATTR static mem_book_t book; // ~1 KB: PSRAM
static portMUX_TYPE                book_mux = portMUX_INITIALIZER_UNLOCKED;

void *lichen_alloc(lichen_app_t const *app, size_t size, uint32_t flags) {
    char const *owner    = app ? app->id : MEM_BOOK_OS;
    bool        internal = (flags & LICHEN_MEM_INTERNAL) != 0;
    if (internal == ((flags & LICHEN_MEM_BULK) != 0) || size == 0 || size > UINT32_MAX) {
        ESP_LOGE(TAG, "%s: lichen_alloc of %u B with flags 0x%x refused: BULK or INTERNAL, one of them", owner,
                 (unsigned)size, (unsigned)flags);
        return NULL;
    }
    portENTER_CRITICAL(&book_mux);
    bool charged = mem_book_charge(&book, owner, internal, (uint32_t)size);
    portEXIT_CRITICAL(&book_mux);
    if (!charged) {
        ESP_LOGW(TAG, "%s: %u B of internal RAM refused: over the app's %u B", owner, (unsigned)size,
                 MEM_BOOK_INTERNAL_QUOTA);
        return NULL;
    }
    uint32_t caps = (internal ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
    void    *p    = (flags & LICHEN_MEM_ZERO) ? heap_caps_calloc(1, size, caps) : heap_caps_malloc(size, caps);
    size_t   got  = p ? heap_caps_get_allocated_size(p) : 0;
    portENTER_CRITICAL(&book_mux);
    if (!p) {
        mem_book_release(&book, owner, internal, (uint32_t)size);
    } else if (got > size) {
        mem_book_adjust(&book, owner, internal, (uint32_t)(got - size)); // the free gives back what the heap says
    }
    portEXIT_CRITICAL(&book_mux);
    return p;
}

void lichen_free(lichen_app_t const *app, void *ptr) {
    if (!ptr) {
        return;
    }
    bool     internal = !esp_ptr_external_ram(ptr);
    uint32_t bytes    = (uint32_t)heap_caps_get_allocated_size(ptr);
    portENTER_CRITICAL(&book_mux);
    mem_book_release(&book, app ? app->id : MEM_BOOK_OS, internal, bytes);
    portEXIT_CRITICAL(&book_mux);
    heap_caps_free(ptr);
}

// The lines "fw mem" adds: what each owner holds through lichen_alloc now, its peak, and refusals
static void mem_report(void) {
    mem_book_t *snap = heap_caps_malloc(sizeof(*snap), MALLOC_CAP_SPIRAM); // 1 KB, off the console's stack
    if (!snap) {
        console_reply("mem apps: out of memory");
        return;
    }
    portENTER_CRITICAL(&book_mux);
    *snap = book;
    portEXIT_CRITICAL(&book_mux);
    for (int i = 0; i < MEM_BOOK_OWNERS; i++) {
        mem_use_t const *r = &snap->row[i];
        if (r->owner[0]) {
            console_reply("mem app %s internal %u (peak %u, quota %u) psram %u (peak %u) blocks %u refused %u", r->owner,
                          (unsigned)r->internal, (unsigned)r->internal_peak, MEM_BOOK_INTERNAL_QUOTA,
                          (unsigned)r->bulk, (unsigned)r->bulk_peak, (unsigned)r->blocks, (unsigned)r->refused);
        }
    }
    heap_caps_free(snap);
}

void lichen_mem_console_register(void) {
    console_set_mem_report(mem_report);
}
