// The app table (#66) behind lichen_app.h: apps register before shell_home(), home and Settings list them. Since #230
// each app has a context (app_ctx.c) and the current one lives in the task's thread-local slot (lichen_sys_app.h).
// Since #520 the table lives in PSRAM, apps can leave again (lichen_unregister_app) and Home and Settings are told.
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"

#include "app_caps.h"
#include "app_ctx.h"
#include "app_ns.h"
#include "app_secret.h"
#include "app_state.h"
#include "panic_ring.h"
#include "ui_cb.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gate_mem.h"
#include "lichen_ui_ops.h"
#include "settings_backup.h"
#include "fw_console.h"
#include "lichen_random.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "shell";

// Slot 0 is IDF's pthread thread-local storage (pthread_local_storage.c); 1 is Lichen's current app
#define CTX_TLS_INDEX 1
// The Kconfig value, not configNUM_THREAD_LOCAL_STORAGE_POINTERS: that one is doubled when deletion callbacks are on,
// and index 1 must be a pointer slot, never a callback's
_Static_assert(CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS > CTX_TLS_INDEX,
               "CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS must be 2 or more (firmware/sdkconfig.defaults)");
// The table's header (the chunk directory and the counters) is internal .bss (~0.2 KB): read from any task and ISR. The
// slots (owner, generation, the index view, each app's block) are PSRAM, in 16-slot chunks that never move or go away:
// the number of apps is bounded by the context handle's 8 slot bits only (app_ctx.h), and costs no internal RAM.
static app_ctx_table_t s_table;
// The one lock of what the OS keeps per app (lichen_sys_app_lock): the index view (lichen_app_count/_at, ctx_at, the
// lookups) against an add or a remove moving it, one add or remove at a time, and the modules' own parts of the apps'
// blocks (lifecycle, badge, memory book). One recursive mutex rather than one each: a StaticSemaphore_t is 92 B of
// internal RAM, and no part is held for long. A mutex, not a spinlock: the chunks and blocks are PSRAM, which no critical
// section may touch (#271). Taken after the LVGL lock, never before; whoever holds it never waits for the LVGL lock.
static StaticSemaphore_t s_app_lock_buf;
static SemaphoreHandle_t s_app_lock;

// The table's storage is zeroed PSRAM through lichen_alloc (gate_mem.h): lichen_os hands its heap over at boot, before
// any app registers. Charged to the OS, whoever registers or unregisters
static void *table_alloc(size_t size) {
    return gate_mem_new_for(lichen_sys_ctx_os(), size);
}

static void table_release(void *p) {
    gate_mem_free(lichen_sys_ctx_os(), p);
}

static app_ctx_table_t *table(void);

// Taken only where the caller may block: not in an interrupt, not inside a critical section (a call from there passes an
// uncontended take silently and would hang under contention), not before the scheduler runs; there the callers read
// without it, which at worst sees an app list half moved or a count one off. True: held, give it back with
// lichen_sys_app_unlock(true).
bool lichen_sys_app_lock(void) {
    (void)table(); // the mutex exists once the table does
    if (!xPortCanYield() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return false;
    }
    xSemaphoreTakeRecursive(s_app_lock, portMAX_DELAY);
    return true;
}

void lichen_sys_app_unlock(bool held) {
    if (held) {
        xSemaphoreGiveRecursive(s_app_lock);
    }
}

// The table's own hooks around the few stores that move the index view: the writers are tasks, which always can block
static void view_lock(void) {
    xSemaphoreTakeRecursive(s_app_lock, portMAX_DELAY);
}

static void view_unlock(void) {
    xSemaphoreGiveRecursive(s_app_lock);
}

static app_ctx_mem_t const s_mem = {.alloc = table_alloc, .release = table_release, .lock = view_lock, .unlock = view_unlock};

// The OS slot is set up on first use, whichever comes first (a registration, or a task asking for its context). The first
// caller does it outside any critical section (it makes the semaphores) while the others wait for it: 0 none, 1 under
// way, 2 done.
static uint32_t s_table_state;

static app_ctx_table_t *table(void) {
    if (__atomic_load_n(&s_table_state, __ATOMIC_ACQUIRE) == 2) {
        return &s_table;
    }
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&s_table_state, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        app_ctx_init_with(&s_table, lichen_random_u32(), &s_mem, APP_CTX_MAX_APPS, sizeof(app_state_t));
        s_app_lock = xSemaphoreCreateRecursiveMutexStatic(&s_app_lock_buf);
        __atomic_store_n(&s_table_state, 2, __ATOMIC_RELEASE);
        return &s_table;
    }
    while (__atomic_load_n(&s_table_state, __ATOMIC_ACQUIRE) != 2) {
        if (xPortCanYield() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
            vTaskDelay(1); // the first caller may be a lower priority on this core
        }
    }
    return &s_table;
}

// An app's backup keys join the SD backup as one group, and only when every key is in the app's own namespace (a group
// naming badge_fw or another app's would let an app export or overwrite what is not its). Feature lost: logged at E (#113).
static void register_backup_keys(lichen_app_t const *app) {
    for (size_t i = 0; i < app->backup_key_count; i++) {
        if (!app_ns_owns(app->id, app->backup_keys[i].ns)) {
            ESP_LOGE(TAG, "app %s: backup key %s is not in its namespace: no backup for the app", app->name,
                     app->backup_keys[i].key);
            return;
        }
    }
    if (!app_secret_keys_plain(app->secret_prefix, app->backup_keys, app->backup_key_count)) {
        ESP_LOGE(TAG, "app %s: a backup key is one of its secrets (#288): no backup for the app", app->name);
        return;
    }
    if (!sb_add_app_keys(app->backup_keys, app->backup_key_count)) {
        ESP_LOGE(TAG, "app %s: its backup keys do not fit the allow-list (SB_KEYS_MAX): no backup for the app", app->name);
    }
}

static void app_names(void const *owner, char const **id, char const **alias) {
    lichen_app_t const *app = owner;
    *id                     = app->id;
    *alias                  = app->alias;
}

static void ring_note(lichen_ctx_t ctx, char const *id, bool pin); // the crash hook's ring of app names (below)
static void ring_drop(lichen_ctx_t ctx);
static void list_changed(void);
typedef struct list_call list_call_t;
typedef enum { LIST_DONE, LIST_PENDING, LIST_FAILED } list_result_t;
static list_result_t list_changed_ex(bool removed, bool wait, void (*done)(void *), void *done_arg);
static bool ui_hold(bool *held);
static void ui_release(bool held);

lichen_ctx_t lichen_register_app(lichen_app_t const *app) {
    app_ctx_table_t *t = table();
    if (!app) {
        ESP_LOGE(TAG, "no app to register");
        return LICHEN_CTX_NONE;
    }
    // A secret prefix the OS cannot enforce would leave the app's secrets as plain settings (#288): not registered
    if (!app_secret_prefix_ok(app->secret_prefix)) {
        ESP_LOGE(TAG, "app %s not registered: its secret prefix is not 1..%d of a-z 0-9 _", app->name ? app->name : "?",
                 APP_SECRET_PREFIX_MAX);
        return LICHEN_CTX_NONE;
    }
    // Two apps sharing an id would share a settings namespace, a directory and a notification source (#340): the
    // second is skipped, never an abort, so the badge still boots with the others
    char const  *refusal = NULL;
    uint32_t     draw    = lichen_random_u32(); // outside the lock
    bool         held;
    if (!ui_hold(&held)) {
        ESP_LOGE(TAG, "app %s not registered: the LVGL lock was not free", app->name ? app->name : "?");
        return LICHEN_CTX_NONE;
    }
    bool         writing = lichen_sys_app_lock(); // one add or remove at a time
    lichen_ctx_t ctx     = app_ctx_add_app(t, app, draw, app_names, &refusal);
    lichen_sys_app_unlock(writing);
    ui_release(held);
    if (refusal) {
        ESP_LOGE(TAG, "app %s (id %s, alias %s) not registered: %s", app->name ? app->name : "?",
                 app->id ? app->id : "-", app->alias ? app->alias : "-", refusal);
    } else if (ctx == LICHEN_CTX_NONE) {
        ESP_LOGE(TAG, "no room for app %s: %d apps are registered already, or no memory for its slot", app->name,
                 APP_CTX_MAX_APPS);
    } else {
        ring_note(ctx, app->id, false);
        if (app->backup_keys) {
            register_backup_keys(app);
        }
        list_changed();
    }
    return ctx;
}

lichen_app_t const *lichen_sys_app_of(lichen_ctx_t ctx) {
    app_ctx_table_t *t = table();
    return (ctx & ((1u << APP_CTX_SLOT_BITS) - 1u)) && app_ctx_valid(t, ctx) ? app_ctx_slot_owner(t, ctx) : NULL;
}

app_state_t *lichen_sys_app_state(lichen_ctx_t ctx) {
    return app_ctx_state(table(), ctx);
}

lichen_unreg_t lichen_unregister_app(lichen_ctx_t ctx, uint32_t *leaked, void (*done)(void *arg), void *done_arg) {
    if (leaked) {
        *leaked = 0;
    }
    bool caller_in_ui = lichen_ui_ops_lock_held(); // before ui_hold takes it: the LVGL task and its lock holders cannot wait
    uint32_t held_blocks = 0, held_internal = 0, held_bulk = 0;
    app_ctx_table_t *t = table();
    bool             held;
    if (!ui_hold(&held)) {
        ESP_LOGW(TAG, "unregister 0x%08lx refused: the LVGL lock was not free", (unsigned long)ctx);
        return LICHEN_UNREG_BUSY;
    }
    bool                writing = lichen_sys_app_lock();
    lichen_app_t const *app     = app_ctx_slot_owner(t, ctx);
    lichen_unreg_t      r   = LICHEN_UNREG_OK;
    if ((ctx & ((1u << APP_CTX_SLOT_BITS) - 1u)) == 0 || !app_ctx_valid(t, ctx) || !app) {
        r = LICHEN_UNREG_BAD_CTX;
    } else if (!lichen_sys_life_removable(ctx)) {
        r = LICHEN_UNREG_RUNNING; // the caller stops the app first
    } else {
        app_state_t *st = app_ctx_state(t, ctx);
        if (st) { // what it still holds in the memory book: the block goes with the app and the book cannot free for it
            held_blocks   = st->mem.blocks;
            held_internal = st->mem.internal;
            held_bulk     = st->mem.bulk;
        }
        ring_drop(ctx); // the crash hook stops naming it before the table forgets it
        r = app_ctx_remove(t, ctx) ? LICHEN_UNREG_OK : LICHEN_UNREG_BAD_CTX;
    }
    lichen_sys_app_unlock(writing);
    ui_release(held);
    if (r != LICHEN_UNREG_OK) {
        ESP_LOGW(TAG, "unregister 0x%08lx refused: %s", (unsigned long)ctx,
                 r == LICHEN_UNREG_RUNNING ? "the app runs, stop it first" : "no such app");
        return r;
    }
    if (leaked) {
        *leaked = held_blocks;
    }
    if (held_blocks) {
        ESP_LOGW(TAG, "app %s left holding %lu lichen_alloc blocks (%lu B internal, %lu B PSRAM): leaked",
                 app->id ? app->id : app->name, (unsigned long)held_blocks, (unsigned long)held_internal,
                 (unsigned long)held_bulk);
    }
    // Its block (lifecycle record, memory row, timers' flags, directories) went with the slot; a timer it booked finds no
    // state and deletes itself at its next tick
    if (app->backup_keys) {
        sb_remove_app_keys(app->backup_keys);
    }
    list_result_t got = list_changed_ex(true, !caller_in_ui, done, done_arg);
    return got == LIST_DONE ? LICHEN_UNREG_OK : got == LIST_PENDING ? LICHEN_UNREG_PENDING : LICHEN_UNREG_UNNOTIFIED;
}

lichen_unreg_t lichen_app_list_renotify(void (*done)(void *arg), void *done_arg) {
    list_result_t got = list_changed_ex(true, !lichen_ui_ops_lock_held(), done, done_arg);
    return got == LIST_DONE ? LICHEN_UNREG_OK : got == LIST_PENDING ? LICHEN_UNREG_PENDING : LICHEN_UNREG_UNNOTIFIED;
}

// ---- the app list changed (#520) -------------------------------------------------------------------------------------
// Home and Settings keep indexes and app pointers; they re-read the list when told. The observers run on the LVGL task
// (queued with lichen_sys_ui_call), after the app is gone from the table: an app struct an unregistered app owned (an SD
// app's, freed by its loader) must not be freed before they ran, see lichen_sys_app.h.
#define LIST_OBSERVERS 4
static struct {
    void (*cb)(void *arg, bool removed);
    void *arg;
} s_observers[LIST_OBSERVERS];
static bool s_removed_pending; // an app left since the observers last ran: indexes of those after it moved
static portMUX_TYPE s_obs_mux = portMUX_INITIALIZER_UNLOCKED;

bool lichen_app_list_observe(void (*cb)(void *arg, bool removed), void *arg) {
    bool ok = false;
    portENTER_CRITICAL_SAFE(&s_obs_mux);
    for (size_t i = 0; i < LIST_OBSERVERS && cb && !ok; i++) {
        if (!s_observers[i].cb) {
            s_observers[i].cb  = cb;
            s_observers[i].arg = arg;
            ok                 = true;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_obs_mux);
    return ok;
}

void lichen_app_list_unobserve(void (*cb)(void *arg, bool removed), void *arg) {
    portENTER_CRITICAL_SAFE(&s_obs_mux);
    for (size_t i = 0; i < LIST_OBSERVERS; i++) {
        if (s_observers[i].cb == cb && s_observers[i].arg == arg) {
            s_observers[i].cb = NULL;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_obs_mux);
}

// Off while the apps register at boot, which happens after the shell started and Home subscribed: no lock, no queued
// rebuild for each of them. lichen_app_list_live turns it on.
static bool s_live;

void lichen_app_list_live(void) {
    __atomic_store_n(&s_live, true, __ATOMIC_RELEASE);
}

// One queued run of the observers. The caller that waits for it and the run hand it over by state: 0 queued, 1 the run is
// over (the waiter frees it), 2 the waiter gave up (the run frees it). A call that nobody waits for is freed by its run.
struct list_call {
    uint32_t          state;
    bool              wait;
    void            (*done)(void *arg); // after the observers, on the LVGL task
    void             *done_arg;
    StaticSemaphore_t sem_buf;
    SemaphoreHandle_t sem;
};

static void list_call_free(list_call_t *c) {
    gate_mem_free(lichen_sys_ctx_os(), c);
}

static void list_changed_run(void *arg) {
    list_call_t *c       = arg;
    bool         removed = __atomic_exchange_n(&s_removed_pending, false, __ATOMIC_ACQ_REL);
    for (size_t i = 0; i < LIST_OBSERVERS; i++) {
        void (*cb)(void *, bool) = NULL;
        void *obs_arg            = NULL;
        portENTER_CRITICAL_SAFE(&s_obs_mux);
        cb      = s_observers[i].cb;
        obs_arg = s_observers[i].arg;
        portEXIT_CRITICAL_SAFE(&s_obs_mux);
        if (cb) {
            cb(obs_arg, removed);
        }
    }
    if (c->done) {
        c->done(c->done_arg);
    }
    uint32_t queued = 0;
    if (!c->wait || !__atomic_compare_exchange_n(&c->state, &queued, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        list_call_free(c); // nobody waits, or the waiter gave up
    } else {
        xSemaphoreGive(c->sem);
    }
}

static bool observers_any(void) {
    bool any = false;
    if (!__atomic_load_n(&s_live, __ATOMIC_ACQUIRE)) {
        return false;
    }
    portENTER_CRITICAL_SAFE(&s_obs_mux);
    for (size_t i = 0; i < LIST_OBSERVERS; i++) {
        any = any || s_observers[i].cb;
    }
    portEXIT_CRITICAL_SAFE(&s_obs_mux);
    return any;
}

// Home and Settings read the list under the LVGL lock, a whole build or timer tick at a time, so the table changes under
// it too: they never see it move halfway. Nobody is told before lichen_app_list_live (the boot registrations): no lock then.
// Lock order: the LVGL lock, then the table's write lock.
static bool ui_hold(bool *held) {
    *held = false;
    if (!observers_any()) {
        return true;
    }
    *held = lichen_ui_ops_lock(5000);
    return *held;
}

static void ui_release(bool held) {
    if (held) {
        lichen_ui_ops_unlock();
    }
}

// Queues the observers (removed: an app left, which they are told). done runs after them on the LVGL task, once (it
// frees what the app owned). wait: the caller is no LVGL task and holds no LVGL lock, so it waits up to 2 s for the run;
// LIST_DONE when the run finished in time (done has run); LIST_PENDING when it is still to come (done runs later, on the
// LVGL task). With nobody listening done runs here, on the caller's thread, at once (LIST_DONE). LIST_FAILED: no memory for
// the call or the LVGL task would not take it: the observers did not run and done will not, so the caller keeps whatever
// done would have freed, and may ask again with lichen_app_list_renotify. No lock is held.
static list_result_t list_changed_ex(bool removed, bool wait, void (*done)(void *), void *done_arg) {
    if (removed) {
        __atomic_store_n(&s_removed_pending, true, __ATOMIC_RELEASE); // set even when nobody listens yet: the next run says so
    }
    if (!observers_any()) {
        if (done) {
            done(done_arg);
        }
        return LIST_DONE;
    }
    list_call_t *c = gate_mem_new_for(lichen_sys_ctx_os(), sizeof(*c));
    if (!c) {
        ESP_LOGW(TAG, "app list change not delivered: no memory");
        return LIST_FAILED;
    }
    c->wait     = wait;
    c->done     = done;
    c->done_arg = done_arg;
    c->sem      = wait ? xSemaphoreCreateBinaryStatic(&c->sem_buf) : NULL;
    if (!lichen_ui_ops_later(list_changed_run, c)) {
        ESP_LOGW(TAG, "app list change not delivered: the LVGL lock was not free");
        list_call_free(c);
        return LIST_FAILED;
    }
    if (!wait) {
        return LIST_PENDING;
    }
    if (xSemaphoreTake(c->sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
        list_call_free(c);
        return LIST_DONE;
    }
    uint32_t queued = 0;
    if (__atomic_compare_exchange_n(&c->state, &queued, 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        ESP_LOGW(TAG, "app list observers did not run in 2 s: done follows on the LVGL task");
        return LIST_PENDING; // the run frees the call
    }
    xSemaphoreTake(c->sem, portMAX_DELAY); // it finished just now
    list_call_free(c);
    return LIST_DONE;
}

static void list_changed(void) {
    list_changed_ex(false, false, NULL, NULL);
}

// The index view (registration order) moves when an app leaves, so every read of it is under the view lock; a pointer
// it returns stays valid until that app is unregistered (the observers say so, lichen_sys_app.h)
size_t lichen_app_count(void) {
    (void)table(); // the mutex exists once the table does
    bool locked = lichen_sys_app_lock();
    size_t n = app_ctx_apps(&s_table);
    lichen_sys_app_unlock(locked);
    return n;
}

lichen_app_t const *lichen_app_at(size_t index) {
    (void)table();
    bool locked = lichen_sys_app_lock();
    lichen_app_t const *app = app_ctx_app_at(&s_table, index);
    lichen_sys_app_unlock(locked);
    return app;
}

lichen_ctx_t lichen_sys_app_ctx_at(size_t index) {
    (void)table();
    bool locked = lichen_sys_app_lock();
    lichen_ctx_t ctx = app_ctx_at(&s_table, index);
    lichen_sys_app_unlock(locked);
    return ctx;
}

// The app named name by its id (alias false) or its alias, NULL for none; *ctx (may be NULL) gets its context. Under
// the view lock.
static lichen_app_t const *find_app(char const *name, bool alias, lichen_ctx_t *ctx) {
    lichen_app_t const *found = NULL;
    (void)table();
    bool locked = lichen_sys_app_lock();
    for (size_t i = 0; name && !found && i < app_ctx_apps(&s_table); i++) {
        lichen_app_t const *app  = app_ctx_app_at(&s_table, i);
        char const         *mine = alias ? app->alias : app->id;
        if (mine && strcmp(mine, name) == 0) {
            found = app;
            if (ctx) {
                *ctx = app_ctx_at(&s_table, i);
            }
        }
    }
    lichen_sys_app_unlock(locked);
    return found;
}

lichen_app_t const *lichen_app_by_id(char const *id) {
    return find_app(id, false, NULL);
}

lichen_app_t const *lichen_app_by_name(char const *name, lichen_ctx_t *ctx) {
    lichen_ctx_t        got = LICHEN_CTX_NONE;
    lichen_app_t const *app = find_app(name, false, &got);
    if (!app) {
        app = find_app(name, true, &got);
    }
    if (ctx) {
        *ctx = app ? got : LICHEN_CTX_NONE;
    }
    return app;
}

// ---- the current app -----------------------------------------------------------------------------------------------

static uint32_t tls_get(void) {
    return (uint32_t)(uintptr_t)pvTaskGetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX);
}

static void tls_set(uint32_t ctx) {
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)ctx);
}

static bool in_isr(void) {
    return xPortInIsrContext();
}

static app_ctx_tls_t const s_tls = {.get = tls_get, .set = tls_set, .in_isr = in_isr};

lichen_ctx_t lichen_ctx_self(void) {
    return app_ctx_self(table(), &s_tls);
}

// ---- the panic handler's view (#232) ---------------------------------------------------------------------------
// The panic hook runs with XIP from PSRAM: a flash function or rodata is fetched through the PSRAM cache, which may
// be what faulted. So lichen_sys_panic_app_id is IRAM_ATTR and reads internal RAM only: a small ring of the ids of the
// apps that ran lately (the table is PSRAM since #520, and a copy per registered app would cost internal RAM per app),
// and the current task's TCB through FreeRTOS's public layout mirror
// (StaticTask_t, whose pvDummy15 is the TCB's thread-local slots). lichen_sys_panic_probe checks that mirror against
// the real call once at boot; until it said yes the hook blames nobody.
extern void *volatile pxCurrentTCBs[]; // FreeRTOS's current task per core (internal .bss)

#define PANIC_PROBE 0x5AFE2320u

// Eight entries (panic_ring.h), filled when an app registers, when a context is entered for a hook and, pinned, when a
// task starts for one. A crash in an app the ring no longer holds blames nobody. Internal .bss (~0.2 KB) and a spinlock
// around the writes, which touch internal RAM only.
static panic_ring_t s_ring;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         s_panic_mirror_ok;

static void ring_note(lichen_ctx_t ctx, char const *id, bool pin) {
    char name[PANIC_RING_ID_CAP]; // a copy: the id may be flash or PSRAM, which the critical section must not read (#271)
    strlcpy(name, id ? id : "", sizeof(name));
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    panic_ring_note(&s_ring, ctx, id ? name : NULL, pin);
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

static void ring_drop(lichen_ctx_t ctx) {
    portENTER_CRITICAL_SAFE(&s_ring_mux);
    panic_ring_drop(&s_ring, ctx);
    portEXIT_CRITICAL_SAFE(&s_ring_mux);
}

// ctx just ran for an app: entered for a hook, or (pin) a task started for it, which runs on its own and so is not
// replaced by hooks entering other apps. The common case, a context already there, is one unlocked scan; only a new
// one (or a pin) reads the app's id (PSRAM or flash, so outside the lock).
static void ring_touch(lichen_ctx_t ctx, bool pin) {
    if (ctx == APP_CTX_NONE || (ctx & ((1u << APP_CTX_SLOT_BITS) - 1u)) == 0 || (!pin && panic_ring_has(&s_ring, ctx))) {
        return;
    }
    lichen_app_t const *app = app_ctx_slot_owner(&s_table, ctx);
    if (app && app->id && app_ctx_valid(&s_table, ctx)) {
        ring_note(ctx, app->id, pin);
    }
}

static void *IRAM_ATTR tcb_slot(void const *tcb) {
    return ((StaticTask_t const *)tcb)->pvDummy15[CTX_TLS_INDEX];
}

bool lichen_sys_panic_probe(void) {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    void        *saved = pvTaskGetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX);
    portENTER_CRITICAL(&mux); // no switch, no migration between the reads
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, (void *)(uintptr_t)PANIC_PROBE);
    void const *tcb = pxCurrentTCBs[esp_cpu_get_core_id()];
    bool        ok  = tcb == (void const *)xTaskGetCurrentTaskHandle() && esp_ptr_in_dram(tcb) &&
             (uintptr_t)tcb_slot(tcb) == PANIC_PROBE;
    vTaskSetThreadLocalStoragePointer(NULL, CTX_TLS_INDEX, saved);
    portEXIT_CRITICAL(&mux);
    s_panic_mirror_ok = ok;
    if (!ok) {
        ESP_LOGE(TAG, "crash attribution off: the task layout is not the one this build expects (#232)");
    }
    return ok;
}

// The table's rules inline (app_ctx_valid lives in flash, the table in PSRAM): a slot of the mirror, generation not 0 and the slot's own
char const *IRAM_ATTR lichen_sys_panic_app_id(void) {
    int core = esp_cpu_get_core_id();
    if (!s_panic_mirror_ok || core < 0 || core >= portNUM_PROCESSORS || xPortInterruptedFromISRContext()) {
        return NULL; // an ISR's fault is not the interrupted task's app's
    }
    void const *tcb = pxCurrentTCBs[core];
    if (!esp_ptr_in_dram(tcb) || ((uintptr_t)tcb & 3u)) {
        return NULL; // a TCB in PSRAM (ALLOW_EXT_MEM) or a wild pointer is never read here
    }
    uint32_t ctx  = (uint32_t)(uintptr_t)tcb_slot(tcb);
    uint32_t slot = ctx & ((1u << APP_CTX_SLOT_BITS) - 1u);
    uint32_t gen  = ctx >> APP_CTX_SLOT_BITS;
    if (slot == 0 || gen == 0) {
        return NULL; // the OS, or no context
    }
    char const *id = panic_ring_find(&s_ring, ctx);
    if (id) {
        return id;
    }
    return NULL; // an app the ring forgot: nobody is blamed (safe_mode counts only crashes it can name)
}

// The task inside an app's LVGL callback and the context it entered (#349, #427; ui_cb.h has the store order), for
// the panic hook: internal .bss, written only by the LVGL task under the LVGL lock
static ui_cb_mark_t s_ui_cb_mark;

uint32_t lichen_sys_ui_cb_mark(bool entering, lichen_ctx_t ctx) {
    if (entering) {
        return ui_cb_mark_enter(&s_ui_cb_mark, xTaskGetCurrentTaskHandle(), ctx);
    }
    ui_cb_mark_leave(&s_ui_cb_mark, ctx);
    return 0;
}

// The same reads as lichen_sys_panic_app_id: the TCB only when it is in internal RAM and the mirror checked out
bool IRAM_ATTR lichen_sys_panic_in_ui_cb(void) {
    int core = esp_cpu_get_core_id();
    if (!s_panic_mirror_ok || core < 0 || core >= portNUM_PROCESSORS || xPortInterruptedFromISRContext()) {
        return false;
    }
    void const *tcb = pxCurrentTCBs[core];
    if (!esp_ptr_in_dram(tcb) || ((uintptr_t)tcb & 3u)) {
        return false;
    }
    return ui_cb_mark_holds(&s_ui_cb_mark, tcb, (uint32_t)(uintptr_t)tcb_slot(tcb));
}

static char const *app_name(void const *owner) {
    lichen_app_t const *app = owner;
    return app ? (app->id ? app->id : app->name) : "the OS";
}

// A refusal the table has counted: logged when the rate limit lets it (never from an ISR), with the app the ctx
// claims to be (its slot's) and the one the caller runs for
static void log_refusal(lichen_ctx_t ctx, char const *what, lichen_ctx_t caller_ctx) {
    uint32_t skipped = 0;
    if (in_isr() || !app_ctx_log_due(&s_table, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()), &skipped)) {
        return;
    }
    void const *claimed = app_ctx_slot_owner(&s_table, ctx);
    void const *caller  = app_ctx_slot_owner(&s_table, caller_ctx);
    char const *claims = claimed || (ctx & 0xffu) == 0 ? app_name(claimed) : "no app";
    ESP_LOGW(TAG, "%s: context 0x%08lx refused (claims %s, called for %s; stale or made up; %lu more not logged "
                  "since the last)", what ? what : "?", (unsigned long)ctx, claims, app_name(caller),
             (unsigned long)skipped);
}

lichen_ctx_t lichen_ctx_enter(lichen_ctx_t ctx) {
    bool         refused  = false;
    lichen_ctx_t previous = app_ctx_enter(table(), &s_tls, ctx, &refused);
    if (refused) {
        log_refusal(ctx, "enter", previous); // the caller's, not the OS's entered instead
    } else if (!in_isr()) {
        ring_touch(ctx, false); // the crash hook can name it now
    }
    return previous;
}

void lichen_ctx_leave(lichen_ctx_t previous) {
    app_ctx_leave(&s_tls, previous);
}

// An app struct that never registered is an ordering slip in the OS: said once per struct (a few remembered)
#define UNREGISTERED_SEEN 4
static void const  *s_unregistered[UNREGISTERED_SEEN];
static portMUX_TYPE s_seen_mux = portMUX_INITIALIZER_UNLOCKED;

static void warn_unregistered(lichen_app_t const *app) {
    bool first = true;
    portENTER_CRITICAL_SAFE(&s_seen_mux);
    for (size_t i = 0; i < UNREGISTERED_SEEN && first; i++) {
        if (s_unregistered[i] == app) {
            first = false;
        } else if (!s_unregistered[i]) {
            s_unregistered[i] = app;
            break;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_seen_mux);
    if (first && !in_isr()) {
        ESP_LOGW(TAG, "app %s is not registered: it runs for the OS", app_name(app));
    }
}

lichen_ctx_t lichen_sys_task_ctx(lichen_app_t const *app) {
    bool             unregistered = false;
    app_ctx_table_t *t            = table();
    bool         locked = lichen_sys_app_lock(); // the index view: an app leaving moves it
    lichen_ctx_t ctx        = app_ctx_for_task(t, &s_tls, app, &unregistered);
    lichen_sys_app_unlock(locked);
    ring_touch(ctx, true); // a task of the app: named for as long as it runs
    if (unregistered) {
        warn_unregistered(app);
    }
    return ctx;
}

lichen_ctx_t lichen_sys_ctx_os(void) {
    return app_ctx_os(table());
}

bool lichen_sys_ctx_resolve(lichen_ctx_t ctx, lichen_app_t const **app, char const *what) {
    void const *owner = NULL;
    if (!app_ctx_resolve(table(), ctx, &owner)) {
        log_refusal(ctx, what, lichen_ctx_self());
        return false;
    }
    if (app) {
        *app = owner;
    }
    return true;
}

// The settings, file, badge and notice calls (#230 E1c): gate_app.c's decisions over the real table
static bool env_resolve(uint32_t ctx, char const **id, char const **name, char const *what) {
    lichen_app_t const *app = NULL;
    if (!lichen_sys_ctx_resolve(ctx, &app, what)) {
        return false;
    }
    *id   = app ? app->id : NULL;
    *name = app ? app->name : NULL;
    return true;
}

// A valid ctx an app-facing call may not use (the OS's): counted and logged like any refusal
static void env_refuse(uint32_t ctx, char const *what) {
    app_ctx_count_refusal(&s_table, ctx);
    log_refusal(ctx, what, lichen_ctx_self());
}

static gate_app_env_t const s_app_env = {.resolve = env_resolve, .os = lichen_sys_ctx_os, .refuse = env_refuse};

gate_app_kind_t lichen_sys_ctx_app(lichen_ctx_t ctx, char const *what, char const **id, char const **name) {
    return gate_app_id(&s_app_env, ctx, what, id, name);
}

gate_app_kind_t lichen_sys_ctx_caller(lichen_ctx_t ctx, char const *what, char const **id, char const **name) {
    return gate_app_caller(&s_app_env, ctx, what, id, name);
}

char const *lichen_sys_ctx_source(lichen_ctx_t ctx, char const *what) {
    return gate_app_source(&s_app_env, ctx, what);
}

gate_app_kind_t lichen_sys_ctx_settings_ns(lichen_ctx_t ctx, char const *what, char *ns, size_t cap,
                                           char const **name) {
    return gate_app_settings_ns(&s_app_env, ctx, what, ns, cap, name);
}

// ---- capabilities (#229) --------------------------------------------------------------------------------------------

// Internal .bss (~0.1 KB): counted from any task, so never PSRAM
static app_caps_refusals_t s_caps_refused;

static uint32_t app_caps(void const *owner) {
    return ((lichen_app_t const *)owner)->caps;
}

bool lichen_sys_caps_ok(lichen_ctx_t ctx, uint32_t need, char const *what) {
    if (app_caps_check(&s_caps_refused, table(), ctx, need, app_caps)) {
        return true;
    }
    uint32_t skipped = 0;
    if (!in_isr() && app_caps_log_due(&s_caps_refused, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount()), &skipped)) {
        char needs[48];
        app_caps_format(need, needs, sizeof(needs));
        ESP_LOGW(TAG, "%s refused for app %s: it needs %s, which the app did not declare (#229; %lu more not logged "
                      "since the last)", what ? what : "?", app_name(app_ctx_slot_owner(&s_table, ctx)), needs,
                 (unsigned long)skipped);
    }
    return false;
}

bool lichen_sys_caps_self_ok(uint32_t need, char const *what) {
    return lichen_sys_caps_ok(lichen_ctx_self(), need, what);
}

// ---- fw app ctx ----------------------------------------------------------------------------------------------------

static void (*s_more)(char const *args); // lichen_sys_app_console_more, set at boot before "app" is registered
static char const *s_more_usage;
static console_sub_t const *s_more_rules;

void lichen_sys_app_console_more(void (*more)(char const *args), char const *usage, console_sub_t const *rules) {
    s_more       = more;
    s_more_usage = usage;
    s_more_rules = rules;
}

static void cmd_app_ctx(void) {
    app_ctx_table_t *t = table();
    console_reply("app ctx os 0x%08lx current 0x%08lx", (unsigned long)app_ctx_os(t), (unsigned long)lichen_ctx_self());
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app) {
            console_reply("app ctx %s 0x%08lx", app->id ? app->id : app->name, (unsigned long)lichen_sys_app_ctx_at(i));
        }
    }
    console_reply("app ctx: %u apps, %lu refused since boot, last refused 0x%08lx", (unsigned)lichen_app_count(),
                  (unsigned long)__atomic_load_n(&t->refused, __ATOMIC_RELAXED),
                  (unsigned long)__atomic_load_n(&t->last_refused, __ATOMIC_RELAXED));
}

// #229: what each app declared (tools/layers.conf mirrors it), its budgets and the calls refused for it since boot
static void cmd_app_caps(void) {
    char names[160];
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        app_state_t        *st  = lichen_sys_app_state(lichen_sys_app_ctx_at(i));
        if (!app || !st) {
            continue; // the app left since the count
        }
        app_caps_format(app->caps, names, sizeof(names));
        console_reply("app caps %s: %s; budget internal %u KiB, PSRAM %u KiB; %lu refused", app->id ? app->id : app->name,
                      names, (unsigned)app->internal_ram_kib, (unsigned)app->psram_kib,
                      (unsigned long)__atomic_load_n(&st->caps_refused, __ATOMIC_RELAXED));
    }
    app_caps_format(__atomic_load_n(&s_caps_refused.last_need, __ATOMIC_RELAXED), names, sizeof(names));
    console_reply("app caps: %lu refused since boot (last needed %s)",
                  (unsigned long)__atomic_load_n(&s_caps_refused.refused, __ATOMIC_RELAXED), names);
}

static void cmd_app(char const *args) {
    if (strcmp(args, "ctx") == 0) {
        cmd_app_ctx();
    } else if (strcmp(args, "caps") == 0) {
        cmd_app_caps();
    } else if (strcmp(args, "life") == 0) {
        lichen_sys_life_console(); // #231
    } else if (s_more) {
        s_more(args);
    } else {
        console_reply("usage: app ctx | app caps | app life");
    }
}

void lichen_sys_app_console_register(void) {
    console_register_rules("app", s_more_usage ? s_more_usage
                                               : "app ctx (every app's context, the refusals of stale or made-up contexts "
                                                 "since boot) | app caps (each app's capabilities, budgets and refused calls) | app life",
                           cmd_app, s_more_rules);
}
