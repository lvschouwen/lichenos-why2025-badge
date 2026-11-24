// lichen_alloc (#181): heap_caps with the placement rule and the per-app count (mem_book.c) on top. The count is under
// a mutex (an allocation can come from any task, and the book's work is a few compares); an app's row is in its block in
// PSRAM (#520), which no critical section may touch (#271). The owner is the caller's context (#230 E1b), checked before
// anything is booked: no LVGL lock.
#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "lichen_sys_mem.h"
#include "app_state.h"
#include "gate_book.h"
#include "lichen_caps.h"
#include "lichen_os.h"

#include "dma_watch.h"
#include "mem_diff_cmd.h"
#include "mem_window_watch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string.h>

static char const TAG[] = "mem";

// The OS's row is here (internal, ~0.1 KB); an app's row is in its block (app_state.h, #520), which is PSRAM and goes with
// the app, so the apps cost no internal RAM and there is no limit on them. Hence a mutex around the book, not a
// spinlock: no critical section may touch PSRAM (#271). The mutex is made on first use.
static mem_book_t        book = {.app_row = NULL}; // set up by book_init
static mem_unbooked_t    s_unbooked;    // blocks allocated where the book cannot be taken (an interrupt, a critical section)
static bool              went_unbooked; // a ctx had no row once (a stale one): frees without a row are expected from then on

static mem_use_t *app_row(void *user, uint32_t ctx) {
    (void)user;
    app_state_t *st = lichen_sys_app_state(ctx);
    return st ? &st->mem : NULL;
}

// Where lichen_sys_app_lock takes the lock (see there), and so where the book may be touched at all (mem_book_can_book).
// Elsewhere lichen_alloc and lichen_free go straight to the heap, unbooked: the rows are PSRAM, which no critical section
// may touch (#271), and an unlocked update could tear one. The same answer at the take and the give: the context does not
// change in between.
static bool book_locking(void) {
    return mem_book_can_book(xPortCanYield(), xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
}

// The app lock (lichen_sys_app_lock, recursive): the book's lookups of an app's row take it again for the app list
static void book_take(void) {
    lichen_sys_app_lock();
}
static void book_give(void) {
    lichen_sys_app_unlock(book_locking());
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
    book_take();
    bool first    = !went_unbooked;
    went_unbooked = true;
    book_give();
    return first;
}

static void *heap_take(size_t align, size_t size, uint32_t flags, bool internal) {
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
    return p;
}

// align 0: the heap's default
static void *alloc_aligned(lichen_ctx_t ctx, size_t align, size_t size, uint32_t flags) {
    bool internal = (flags & LICHEN_MEM_INTERNAL) != 0;
    if (internal == ((flags & LICHEN_MEM_BULK) != 0) || size == 0 || size > UINT32_MAX) {
        ESP_LOGE(TAG, "ctx 0x%08lx: lichen_alloc of %u B with flags 0x%x refused: BULK or INTERNAL, one of them",
                 (unsigned long)ctx, (unsigned)size, (unsigned)flags);
        return NULL;
    }
    if (!book_locking()) { // an interrupt or a critical section: no book, no ctx lookup (PSRAM), the heap alone
        void *p = heap_take(align, size, flags, internal);
        if (p) {
            mem_unbooked_note(&s_unbooked);
        }
        return p;
    }
    gate_owner_t who;
    mem_charge_t charge = gate_mem_charge(&env, &book, ctx, internal, (uint32_t)size, &who);
    if (charge == MEM_GATE_REFUSED) {
        return NULL; // logged and counted by the context table
    }
    char const *owner = who.label ? who.label : MEM_BOOK_OS;
    if (charge == MEM_CHARGE_QUOTA) {
        // PSRAM is refused only where its quota is enforced, which the firmware does not do yet (#231)
        ESP_LOGW(TAG, "%s: %u B of %s refused: over the app's quota", owner, (unsigned)size,
                 internal ? "internal RAM" : "PSRAM");
        return NULL;
    }
    if (charge == MEM_CHARGE_NO_ROW && first_unbooked()) {
        // Bookkeeping never fails an allocation: this owner's memory is not counted (logged once)
        ESP_LOGW(TAG, "%s: no row in the memory book (its app is gone?): allocated, but not counted", owner);
    }
    void    *p    = heap_take(align, size, flags, internal);
    size_t   got  = p ? heap_caps_get_allocated_size(p) : 0;
    if (charge == MEM_CHARGE_OK) {
        book_take();
        if (!p) {
            mem_book_release(&book, ctx, internal, (uint32_t)size);
        } else if (got > size) {
            mem_book_adjust(&book, ctx, internal, (uint32_t)(got - size)); // the free gives back what the heap says
        }
        book_give();
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
    if (!book_locking()) { // as the allocation: straight to the heap, the book untouched
        heap_caps_free(ptr);
        return;
    }
    char const   *label = NULL;
    uint32_t      bytes = 0;
    mem_release_t rel   = gate_mem_release(&env, &book, ctx, ptr, &label, &bytes);
    if (rel == MEM_GATE_RELEASE_REFUSED) {
        return; // refused (logged, counted) before ptr was looked at: the block, if it is one, stays allocated
    }
    char const *owner = label ? label : MEM_BOOK_OS;
    book_take();
    bool unbooked = went_unbooked;
    book_give();
    if (rel != MEM_RELEASE_OK && mem_unbooked_absorb(&s_unbooked, (int)rel)) {
        rel = MEM_RELEASE_OK; // one of the unbooked blocks: no mismatch to report
    }
    if (rel == MEM_RELEASE_OVER || (rel == MEM_RELEASE_NO_ROW && !unbooked)) {
        // Freed under another owner than it was allocated with, or never from lichen_alloc: some count is now wrong
        ESP_LOGW(TAG, "%s: lichen_free of %p (%u B) is more than it holds: freed under the wrong owner?", owner, ptr,
                 (unsigned)bytes);
    }
    heap_caps_free(ptr);
}

// The lines "fw mem" adds: what each owner holds through lichen_alloc now, its peak, and refusals
static void mem_report(void) {
    size_t apps = lichen_app_count();
    for (size_t i = 0; i <= apps; i++) { // the OS's row, then every app's
        mem_use_t row; // one row at a time onto the stack
        book_take();
        mem_use_t const *src = i == 0 ? &book.os : app_row(NULL, lichen_sys_app_ctx_at(i - 1));
        if (src) {
            row = *src;
        } else {
            row.owner[0] = '\0'; // the app left since the count
        }
        book_give();
        mem_use_t const *r = &row;
        if (r->owner[0]) {
            console_reply("mem app %s internal %u (peak %u, quota %u) psram %u (peak %u) blocks %u refused %u", r->owner,
                          (unsigned)r->internal, (unsigned)r->internal_peak, MEM_BOOK_INTERNAL_QUOTA,
                          (unsigned)r->bulk, (unsigned)r->bulk_peak, (unsigned)r->blocks, (unsigned)r->refused);
            if (r->bulk_flags & MEM_BOOK_BULK_QUOTA) {
                console_reply("mem app %s psram quota %u (%s) passed %u time(s)", r->owner, (unsigned)r->bulk_quota,
                              (r->bulk_flags & MEM_BOOK_BULK_ENFORCE) ? "enforced" : "accounted", (unsigned)r->bulk_over);
            }
        }
    }
    dma_watch_report();
}

// #231: every app's declared PSRAM budget (lichen_app_t.psram_kib, tools/layers.conf) as its PSRAM quota, accounted and
// not enforced: the budgets were measured as statics and stacks (#229), the heap part is not measured yet, so a refusal
// would fail an app that works today. SYSTEM apps (the shell's) declare none.
bool lichen_sys_mem_quota_set(lichen_ctx_t ctx, uint32_t psram_kib) {
    lichen_app_t const *app = lichen_sys_app_of(ctx);
    if (!app || (app->caps & CAP_SYSTEM)) {
        return false;
    }
    book_take();
    bool ok = mem_book_set_bulk_quota(&book, ctx, app->id, psram_kib * 1024u, false);
    book_give();
    if (!ok) {
        ESP_LOGW(TAG, "%s: no row in the memory book for its PSRAM quota", app->id ? app->id : app->name);
    }
    return ok;
}

void lichen_sys_mem_quotas(void) {
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app) {
            lichen_sys_mem_quota_set(lichen_sys_app_ctx_at(i), app->psram_kib);
        }
    }
}

bool lichen_os_mem_held(uint32_t ctx, uint32_t *internal, uint32_t *bulk) {
    book_take();
    mem_use_t const *r = mem_book_find(&book, ctx);
    *internal          = r ? r->internal : 0;
    *bulk              = r ? r->bulk : 0;
    book_give();
    return r != NULL;
}

// The gate's widgets allocate through these (gate_mem.c, #296): the gate may not link to lichen_os
static lichen_sys_mem_ops_t const gate_ops = {.alloc = lichen_alloc, .release = lichen_free};

void lichen_sys_mem_start(void) {
    book.app_row = app_row; // before the first booking: the apps' rows are in their blocks
    lichen_sys_mem_ops_set(&gate_ops);
}

void lichen_mem_console_register(void) {
    console_set_mem_report(mem_report);
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    console_set_mem_owner(dma_watch_owner);
#endif
    mem_window_watch_register();
    dma_watch_start(); // its first sample also starts the mem window
}
