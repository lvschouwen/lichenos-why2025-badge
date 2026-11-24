// The SD apps' loader service (#523, epic #516). One task of its own ("sdapps", never the console's, OTA's or LVGL's)
// does everything slow, one request at a time: the mount scan, loading an ELF, unloading it, removing apps with the card.
//
// An app is a tile from the scan, with no code loaded. Opening the tile (the trampoline, LVGL task) builds a placeholder
// screen and queues a load; after the open animation the loader reads and relocates the ELF, starts main() on a task of
// the app's own (booked under its context) and waits for it to hand over its hooks (lichen_sd_app_hooks); the hooks' open
// then runs on the LVGL task into the screen, if the screen is still there. When the app's last screen leaves the stack
// the loader waits a moment, then unloads in the order of sd_unload_next: the stop hook, the app's tasks, its timers and
// queued UI calls (a barrier call through the LVGL task), and only then esp_elf_deinit. A card that goes takes the same road
// to the stop, then lichen_unregister_app, and frees the image only after the shell closed the screens (its done callback).
//
// Locking: sd_app_lock is a leaf mutex for the records' states and is never held across a call that may wait or take
// another lock. ELF hooks are called on the LVGL task (open) and the loader (stop) only, never with sd_app_lock held, and
// never once the state left LOADED; the unload's barrier call queues behind any hook that runs on the LVGL task.
#include "sd_apps.h"

#include "export_blocks.h"
#include "export_file.h"
#include "ptr_map.h"
#include "lichen_exports.h"
#include "lichen_mem.h"
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"
#include "lichen_sys_notify.h"
#include "lichen_os.h"
#include "lichen_sys_ui.h"
#include "lichen_ui.h"
#include "lichen_ui_ops.h"
#include "lichen_task.h"
#include "lichen_task_os.h"
#include "sd_app_svc.h"
#include "sd_elf_src.h"
#include "sdcard.h"
#include "theme.h"
#include "esp_timer.h"

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "safe_mode.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "sdapps";

#define LOADER_STACK       6144 // the loader's own: manifests and paths on it, no big buffers
#define OPEN_SETTLE_MS     450  // the ELF loads after the open animation: its cache flush is the #31 window
#define HOOKS_WAIT_MS      5000 // main() has this long to call lichen_sd_app_hooks
#define TASKS_WAIT_MS      3000 // after the stop hook, the app's tasks have this long to return
#define PROGRESS_LOCK_MS   5    // a bar update never holds the loader up (0 would wait for ever)
#define LATE_PATIENCE_MS   30000 // a resumable fault older than this answers as a dead app (a hung task never ends)
#define UI_WAIT_MS         3000 // the barrier call through the LVGL task
#define SD_WAIT_MS         2000
#define UNREG_TRIES        20
#define STACK_KIB_DEFAULT  4

typedef enum { MSG_CARD, MSG_LOAD, MSG_CLOSED, MSG_FREE, MSG_LATE } msg_kind_t;
typedef struct {
    msg_kind_t    kind;
    sd_app_rec_t *rec;
    lichen_ctx_t  ctx; // MSG_LATE: by ctx, the record may be gone by the time it is read
} msg_t;

static SemaphoreHandle_t g_mu;
static SemaphoreHandle_t g_hooks_sem; // main() gave its hooks, or returned
static SemaphoreHandle_t g_blk_mu;    // the apps' block map (#536): a leaf, never held over a call that takes another lock
static SemaphoreHandle_t g_ui_sem;    // the barrier call ran
static QueueHandle_t     g_queue;
#define QUEUE_LEN (3 * SD_APPS_MAX + 4 + 4) // per app a load, a close, a free and a late unload, plus the card events
static sd_app_rec_t     *g_recs; // the loader task adds and removes; others read under g_mu
static _Atomic bool      g_started;
static _Atomic int  g_late_apps;  // apps FAULTED and resumable: the task-exit hook does nothing while it is 0
static _Atomic bool g_late_retry; // a MSG_LATE did not fit the queue

void sd_app_lock(void) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
}
void sd_app_unlock(void) {
    xSemaphoreGive(g_mu);
}

// Is there an app with this id that is not gone (a faulted one stays registered until the badge restarts)?
bool sd_app_known(char const *id) {
    sd_app_lock();
    sd_app_rec_t *r = g_recs;
    while (r && (r->gone || strcmp(r->m.id, id) != 0)) {
        r = r->next;
    }
    sd_app_unlock();
    return r != NULL;
}

sd_app_rec_t *sd_app_find(lichen_ctx_t ctx) {
    if (!g_mu || ctx == LICHEN_CTX_NONE) {
        return NULL;
    }
    sd_app_lock();
    sd_app_rec_t *r = g_recs;
    while (r && !(r->ctx == ctx && !r->gone)) {
        r = r->next;
    }
    sd_app_unlock();
    return r;
}

void sd_app_link(sd_app_rec_t *rec) {
    sd_app_lock();
    rec->next = g_recs;
    g_recs    = rec;
    sd_app_unlock();
}

static void unlink_rec(sd_app_rec_t *rec) {
    sd_app_lock();
    for (sd_app_rec_t **pp = &g_recs; *pp; pp = &(*pp)->next) {
        if (*pp == rec) {
            *pp = rec->next;
            break;
        }
    }
    sd_app_unlock();
}

static bool post(msg_kind_t kind, sd_app_rec_t *rec) {
    msg_t m = {kind, rec, LICHEN_CTX_NONE};
    return g_queue && xQueueSend(g_queue, &m, 0) == pdTRUE; // never waits: callers hold locks or are the LVGL task
}

// ---- the LVGL side ---------------------------------------------------------------------------------------------------

static void say(lv_obj_t *screen, char const *text) {
    lv_obj_clean(screen);
    lv_obj_t *l = lv_label_create(screen);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
}

static void late_arm(sd_app_rec_t *rec);

// One of the app's screens is being deleted (LVGL's delete event, LVGL task): the record forgets a screen that waited for
// its hooks, and when it was the last one the loader is told. The event comes before the widgets go, so the loader's barrier
// call (queued behind this deletion) is what says they are gone; only then may the image be freed (#523).
static void screen_deleted(lv_event_t *e) {
    sd_app_rec_t *rec = lv_event_get_user_data(e);
    sd_app_lock();
    if (rec->screen == lv_event_get_target(e)) {
        rec->screen = NULL;
    }
    bool plan = sd_app_screen_deleted(&rec->sda);
    bool free_now = rec->free_pending && sd_app_may_free(&rec->sda);
    sd_app_unlock();
    if (free_now && !post(MSG_FREE, rec)) {
        ESP_LOGE(TAG, "%s: free not queued", rec->m.id);
    }
    if (plan && !post(MSG_CLOSED, rec)) {
        ESP_LOGE(TAG, "%s: queue full, stays loaded", rec->m.id);
    }
    late_arm(rec); // a faulted app whose last screen just left may be ready for its late unload
}

// The bar and its text are forgotten when LVGL deletes them (the app's open cleans the screen): a queued update then finds NULL
static void obj_deleted(lv_event_t *e) {
    void **slot = lv_event_get_user_data(e);
    sd_app_lock();
    if (*slot == lv_event_get_target(e)) {
        *slot = NULL;
    }
    sd_app_unlock();
}

// Under the placeholder text: an amber fill on the track, as the Keyboard test's hold bar (#548)
static void add_bar(sd_app_rec_t *rec, lv_obj_t *screen) {
    lv_obj_t *label = lv_obj_get_child(screen, -1);
    lv_obj_t *bar   = lv_bar_create(screen);
    lv_obj_set_size(bar, LV_PCT(100), 10);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    sd_app_lock();
    rec->bar   = bar;
    rec->label = label;
    int pct    = rec->pct; // a screen opened mid-load starts where the load is
    sd_app_unlock();
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    if (pct >= 100) {
        lv_label_set_text(label, "Starting...");
    }
    lv_obj_add_event_cb(bar, obj_deleted, LV_EVENT_DELETE, &rec->bar);
    lv_obj_add_event_cb(label, obj_deleted, LV_EVENT_DELETE, &rec->label);
}

// LVGL task: the percent the loader last handed over, and "Starting..." once the file is read
static void progress_cb(void *arg) {
    sd_app_rec_t *rec = arg;
    sd_app_lock();
    lv_obj_t *bar   = rec->bar;
    lv_obj_t *label = rec->label;
    int       pct   = rec->pct;
    sd_app_unlock();
    if (bar) {
        lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    }
    if (label && pct >= 100) {
        lv_label_set_text(label, "Starting...");
    }
}

// Loader task, on every read of the file: never LVGL itself, only a queued call. It waits at most a few ms for the lock;
// an update that finds it busy is dropped and offered again by the next read. Booked under the OS, not the app whose context
// the read runs in: the bar is the OS's.
static void read_progress(void *arg, size_t done, size_t total) {
    sd_app_rec_t *rec = arg;
    uint32_t      now = (uint32_t)(esp_timer_get_time() / 1000);
    int           pct;
    if (!sd_progress_due(&rec->prog, done, total, now, &pct)) {
        return;
    }
    sd_app_lock();
    rec->pct = pct;
    sd_app_unlock();
    lichen_ctx_t prev = lichen_ctx_enter(lichen_sys_ctx_os());
    bool         sent = lichen_sys_ui_call(progress_cb, rec, PROGRESS_LOCK_MS);
    lichen_ctx_leave(prev);
    if (sent) {
        sd_progress_sent(&rec->prog, pct, now);
    }
}

static void refused_say(sd_app_rec_t *rec, char const *why) {
    ESP_LOGW(TAG, "%s: refused: %s", rec->m.id, why);
    lichen_sys_notify(LN_WARN, rec->m.name, why, 0);
}

void sd_app_tramp_open(lv_obj_t *screen) {
    sd_app_rec_t *rec = sd_app_find(lichen_ctx_self());
    if (!rec) {
        say(screen, "This app is not available");
        return;
    }
    char const     *hold = console_usb_hold_why();
    lichen_sd_hooks_t h  = {0};
    sd_app_lock();
    if (rec->sda.state == SD_APP_FAULTED && lichen_sys_life_now_ms() - rec->fault_ms > LATE_PATIENCE_MS) {
        sd_app_late_expire(&rec->sda);
    }
    sd_open_t v = sd_app_open(&rec->sda, hold);
    if (v == SD_OPEN_LOAD || v == SD_OPEN_WAIT) {
        rec->screen = screen;
        if (v == SD_OPEN_LOAD) {
            rec->pct = 0;
        }
    } else if (v == SD_OPEN_NOW && rec->hooks_set) {
        h = rec->hooks;
    }
    sd_app_unlock();
    if (v == SD_OPEN_NOW || v == SD_OPEN_LOAD || v == SD_OPEN_WAIT) { // counted in screens: its deletion is told
        lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, rec);
    }
    switch (v) {
    case SD_OPEN_NOW:
        if (h.open) {
            h.open(screen); // the shell entered the app's context
        }
        return;
    case SD_OPEN_LOAD:
    case SD_OPEN_WAIT:
        say(screen, "Loading...");
        add_bar(rec, screen);
        if (v == SD_OPEN_LOAD && !post(MSG_LOAD, rec)) {
            sd_app_lock();
            sd_app_load_done(&rec->sda, false);
            sd_app_unlock();
            say(screen, "Loader busy: try again");
        }
        return;
    case SD_OPEN_HELD: {
        char text[SD_APP_MSG_LEN];
        snprintf(text, sizeof(text), "%s (%s)", sd_open_text(v), hold ? hold : "hold");
        say(screen, text);
        refused_say(rec, text);
        return;
    }
    default:
        say(screen, sd_open_text(v));
        refused_say(rec, sd_open_text(v));
        return;
    }
}

// The load ended well and the screen may still be there: the app builds into it (LVGL task)
static void open_cb(void *arg) {
    sd_app_rec_t     *rec = arg;
    lichen_sd_hooks_t h;
    lv_obj_t         *screen;
    sd_app_lock();
    bool live = sd_app_hooks_live(&rec->sda) && !rec->sda.closing && !rec->sda.removing && rec->screen;
    h         = rec->hooks;
    screen    = rec->screen;
    sd_app_unlock();
    if (!live) {
        return; // closed or gone meanwhile: the unload follows
    }
    lv_obj_clean(screen); // the placeholder
    lichen_ctx_t prev = lichen_ctx_enter(rec->ctx);
    h.open(screen);
    lichen_ctx_leave(prev);
}

static void show_msg_cb(void *arg) {
    sd_app_rec_t *rec = arg;
    sd_app_lock();
    lv_obj_t *screen = rec->screen;
    sd_app_unlock();
    if (screen) {
        say(screen, rec->msg);
    }
}

// ---- the hooks an app hands over ---------------------------------------------------------------------------------------

bool lichen_sd_app_hooks(lichen_sd_hooks_t const *h) {
    if (!h || h->size < offsetof(lichen_sd_hooks_t, open) + sizeof(h->open) || !h->open) {
        return false;
    }
    sd_app_rec_t *rec = sd_app_find(lichen_ctx_self());
    if (!rec) {
        return false; // not inside a load (the OS's own context: "fw elf run")
    }
    sd_app_lock();
    bool ok = rec->sda.state == SD_APP_LOADING && !rec->hooks_set;
    if (ok) {
        memset(&rec->hooks, 0, sizeof(rec->hooks));
        memcpy(&rec->hooks, h, h->size < sizeof(rec->hooks) ? h->size : sizeof(rec->hooks));
        rec->hooks.size = sizeof(rec->hooks);
        if (h->size < offsetof(lichen_sd_hooks_t, stop) + sizeof(h->stop)) {
            rec->hooks.stop = NULL; // an older struct does not cover it
        }
        rec->hooks_set = true;
    }
    sd_app_unlock();
    if (ok) {
        xSemaphoreGive(g_hooks_sem);
    }
    return ok;
}

// ---- unload ----------------------------------------------------------------------------------------------------------

static void barrier_cb(void *arg) {
    lichen_ctx_t ctx = (lichen_ctx_t)(uintptr_t)arg;
    if (ctx != LICHEN_CTX_NONE) {
        lichen_sys_timer_drop(ctx);
        lichen_sys_ui_timers_drop(ctx); // also the app's unbooked timers: none ticks into the image after this
    }
    xSemaphoreGive(g_ui_sem);
}

// A call through the LVGL task, after every call queued before it (FIFO) and after any hook running there: the app's
// booked timers are dropped (ctx) and its queued UI calls have run, so no callback of it is left in LVGL's queue
static bool ui_barrier(lichen_ctx_t ctx) {
    xSemaphoreTake(g_ui_sem, 0); // a late answer of an earlier, timed-out one
    if (ctx != LICHEN_CTX_NONE) {
        lichen_sys_timer_kill(ctx);
    }
    if (!lichen_sys_ui_call(barrier_cb, (void *)(uintptr_t)ctx, UI_WAIT_MS)) {
        return false;
    }
    if (xSemaphoreTake(g_ui_sem, pdMS_TO_TICKS(UI_WAIT_MS)) != pdTRUE) {
        return false;
    }
    // The barrier only orders the calls queued before it: a timer callback that ran meanwhile, or a call past the queue's
    // size (a call of its own to LVGL), may still be pending. The app has no task and no timer left, so after this cancel
    // (under the LVGL lock, so a callback running has returned) no code of the app runs from the UI service (#541)
    return ctx == LICHEN_CTX_NONE || lichen_sys_ui_cancel_owner(ctx, UI_WAIT_MS);
}

// The stop hook runs on a task of its own (the app's stack_kib, booked under the app), never on the loader's 6 KB or the
// OTA task's stack, and the caller waits for it up to the unload deadline: a hook that hangs faults the app instead of the
// loader. Once per load: the update's stop and the unload may both ask, the second finds it called.
static uint32_t stack_bytes_of(sd_app_rec_t const *rec) {
    return (rec->m.stack_kib ? rec->m.stack_kib : STACK_KIB_DEFAULT) * 1024u;
}

static void stop_task(void *arg) {
    sd_app_rec_t *rec = arg;
    rec->stop_ok      = rec->hooks.stop();
    xSemaphoreGive(rec->stop_sem);
}

static bool sd_app_stop_hook(sd_app_rec_t *rec, bool unloading) {
    sd_app_lock();
    bool call = (unloading || sd_app_hooks_live(&rec->sda)) && rec->hooks_set && !rec->stop_called && rec->hooks.stop;
    if (call) {
        rec->stop_called = true;
        rec->stop_ok     = false;
    }
    sd_app_unlock();
    if (!call) {
        return true; // not loaded, no hook, or already stopped: nothing to stop but main() returning
    }
    xSemaphoreTake(rec->stop_sem, 0);
    uint32_t t0 = lichen_sys_life_now_ms();
    if (!lichen_os_task_start_for(rec->ctx, "sdapp-stop", stop_task, rec, stack_bytes_of(rec),
                                  LICHEN_PRIO_BACKGROUND, LICHEN_STACK_INTERNAL, NULL)) {
        ESP_LOGE(TAG, "%s: no stop task", rec->m.id);
        return false;
    }
    bool done = xSemaphoreTake(rec->stop_sem, pdMS_TO_TICKS(app_life_deadline_ms(LIFE_STOP_UNLOAD))) == pdTRUE;
    if (!done || !rec->stop_ok) {
        ESP_LOGE(TAG, "%s: stop hook %s after %u ms", rec->m.id, done ? "failed" : "hung",
                 (unsigned)(lichen_sys_life_now_ms() - t0));
    }
    return done && rec->stop_ok;
}

// An update is coming: the app's stop hook, if it is loaded and not stopped yet. The OTA task; the image is not unloaded
// (the badge restarts)
bool sd_app_tramp_stop(void) {
    sd_app_rec_t *rec = sd_app_find(lichen_ctx_self());
    return !rec || sd_app_stop_hook(rec, false);
}

static bool unload_step(sd_app_rec_t *rec, sd_unload_step_t step) {
    switch (step) {
    case SD_UNLOAD_STOP:
        return sd_app_stop_hook(rec, true);
    case SD_UNLOAD_TASKS:
        return lichen_task_leftovers(rec->ctx, TASKS_WAIT_MS) == 0; // the stop task is booked under the app too
    case SD_UNLOAD_UI:
        return ui_barrier(rec->ctx);
    default:
        return true;
    }
}

// What a leaving app may still hold in the OS, given back by the loader: its open files (#533) and the raw keys (#534, through
// the shell's function, lichen_sys_app_leave). Before the image goes (an open file's descriptor outlives main(); a raw-key
// listener calls into the image) and when the card goes. The context may already be stale: both are keyed by the number only.
// False: something may still call into the app, so its image is not freed (the app counts as one that did not stop).
static bool release_app_holdings(sd_app_rec_t *rec) {
    lichen_fs_sd_release_ctx(rec->ctx); // a batch it left open locks the card for everyone (only logged when its task is gone)
    export_file_close_all(rec->ctx);
    bool ok = lichen_sys_app_leave(rec->ctx); // takes the LVGL lock: never under sd_app_lock or the app lock
    if (!ok) {
        ESP_LOGE(TAG, "%s: its raw keys could not be released: image kept", rec->m.id);
    }
    return ok;
}

// ---- the app's malloc blocks (#536) ---------------------------------------------------------------------------------

static void *table_alloc(size_t bytes) {
    return heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static ptr_map_env_t const blk_env = {table_alloc, heap_caps_free};
// One map for every app, behind g_blk_mu, a leaf: nothing else is taken or called with it held but the table's own allocation
// (heap lock under the mutex; no heap code calls the shims, so never the other way round). The owner is the app's context value
// (slot and generation), so a block is the app's whatever context frees it, and a slot reused later is another owner.
static ptr_map_t g_blocks;
static _Atomic bool g_forget_unsafe; // a free ran where the mutex cannot be taken: the map may keep a stale entry

// The shims' side. A malloc under the OS context (a delete callback after the card went) notes nothing: that block stays the
// app's to free. Not from an interrupt or a critical section (an SD app's code never runs there): a note is then skipped
// (the block is the app's to free), a free sets g_forget_unsafe, which makes the sweep leak instead of risking a double free.
void sd_app_block_note(lichen_ctx_t ctx, void *ptr) {
    if (!g_blk_mu || ctx == LICHEN_CTX_NONE || !xPortCanYield()) {
        return;
    }
    xSemaphoreTake(g_blk_mu, portMAX_DELAY);
    ptr_map_note(&g_blocks, &blk_env, ptr, ctx); // no memory for the table: the block stays the app's to free
    xSemaphoreGive(g_blk_mu);
}

void sd_app_block_forget(void *ptr) {
    if (!g_blk_mu) {
        return;
    }
    if (!xPortCanYield()) {
        atomic_store(&g_forget_unsafe, true); // no logging from here
        return;
    }
    xSemaphoreTake(g_blk_mu, portMAX_DELAY);
    ptr_map_forget(&g_blocks, ptr);
    xSemaphoreGive(g_blk_mu);
}

static uint32_t release_block(sd_app_rec_t *rec, void *p) {
    uint32_t bytes = (uint32_t)heap_caps_get_allocated_size(p);
    sd_app_lock();
    bool stale = rec->gone;
    sd_app_unlock();
    if (stale) {
        heap_caps_free(p); // the context is stale (unregistered): its row is gone and was logged as leaked
    } else {
        lichen_free(rec->ctx, p); // under its context, so its row in the book goes down too
    }
    return bytes;
}

typedef struct {
    sd_app_rec_t *rec;
    uint32_t      bytes;
    bool          unsafe;
} sweep_t;

static void sweep_lock(void *u) {
    (void)u;
    xSemaphoreTake(g_blk_mu, portMAX_DELAY);
}
static void sweep_unlock(void *u) {
    (void)u;
    xSemaphoreGive(g_blk_mu);
}
static void sweep_free(void *u, void *p) {
    sweep_t *sw = u;
    if (!sw->unsafe) { // an entry may be stale if a free ran where the mutex was out of reach: leak it then
        sw->bytes += release_block(sw->rec, p);
    }
}

// What the app left (an exit() or a fault never runs its own free). Only called where the image is freed: no task, timer, queued
// call or screen of the app is left, so nothing of it can still call malloc or free. One entry at a time under the mutex, the
// free outside it (ptr_map_sweep).
static void sweep_blocks(sd_app_rec_t *rec) {
    sweep_t                   sw  = {rec, 0, atomic_load(&g_forget_unsafe)};
    ptr_map_sweep_env_t const env = {&sw, sweep_lock, sweep_unlock, sweep_free};
    uint32_t                  n   = (uint32_t)ptr_map_sweep(&g_blocks, rec->ctx, &env);
    sweep_lock(NULL);
    ptr_map_trim(&g_blocks, &blk_env);
    sweep_unlock(NULL);
    if (n && sw.unsafe) {
        ESP_LOGE(TAG, "%s: %u leftover blocks leaked (a free ran where they could not be tracked)", rec->m.id, (unsigned)n);
    } else if (n) {
        ESP_LOGW(TAG, "%s: freed %u leftover blocks (%u B)", rec->m.id, (unsigned)n, (unsigned)sw.bytes);
    }
}

static void free_image(sd_app_rec_t *rec) {
    sweep_blocks(rec);
    sd_app_lock();
    bool enter = sd_app_free_enters(rec->gone);
    sd_app_unlock();
    // #532: after lichen_unregister_app the context is stale; entering it was refused and logged on every card removal
    lichen_ctx_t prev = enter ? lichen_ctx_enter(rec->ctx) : LICHEN_CTX_NONE;
    esp_elf_deinit(&rec->elf);
    if (enter) {
        lichen_ctx_leave(prev);
    }
    memset(&rec->elf, 0, sizeof(rec->elf));
    sd_app_lock();
    rec->image     = false;
    rec->hooks_set = false;
    memset(&rec->hooks, 0, sizeof(rec->hooks)); // no pointer into the freed image survives
    sd_app_unlock();
}

// An unload gave up at a step that only missed a deadline (the stop hook, the tasks): the loader finishes it when the app's
// last task has ended (#541). Any task, never waits: the claim (sd_app_late_claim) lets one message through between the
// task-exit hook, the unload's own check and a screen's delete event. A message that could not be queued is retried by the
// loader (g_late_retry).

static void late_queue(sd_app_rec_t *rec) {
    sd_app_lock();
    bool claim = sd_app_late_claim(&rec->sda);
    sd_app_unlock();
    if (!claim) {
        return;
    }
    msg_t m = {MSG_LATE, NULL, rec->ctx};
    if (!g_queue || xQueueSend(g_queue, &m, 0) != pdTRUE) {
        sd_app_lock();
        sd_app_late_unclaim(&rec->sda);
        sd_app_unlock();
        atomic_store(&g_late_retry, true);
    }
}

static void late_arm(sd_app_rec_t *rec) {
    if (atomic_load(&g_late_apps) != 0 && lichen_os_task_count(rec->ctx) == 0) {
        late_queue(rec);
    }
}

// The retry of a failed post, on the loader: every resumable app with no task left is asked again
static void late_sweep(void) {
    atomic_store(&g_late_retry, false);
    sd_app_lock();
    lichen_ctx_t ctxs[SD_APPS_MAX];
    size_t       n = 0;
    for (sd_app_rec_t *r = g_recs; r && n < SD_APPS_MAX; r = r->next) {
        if (r->sda.state == SD_APP_FAULTED && r->sda.late && !r->gone) {
            ctxs[n++] = r->ctx;
        }
    }
    sd_app_unlock();
    for (size_t i = 0; i < n; i++) {
        sd_app_rec_t *rec = sd_app_find(ctxs[i]);
        if (rec) {
            late_arm(rec);
        }
    }
}

// A task of any context ended and its context has none left (lichen_task.c, on the ending task: no waiting, nothing freed
// here). Only a message to the loader, and only for an app that gave its unload up; every other context returns at once, with
// no lock taken.
static void task_ended(lichen_ctx_t ctx) {
    if (atomic_load(&g_late_apps) == 0) {
        return;
    }
    sd_app_rec_t *rec = sd_app_find(ctx);
    if (rec) {
        late_queue(rec); // the book says no task of ctx is left
    }
}

// The app's state is UNLOADING: the steps in order from `from` (SD_UNLOAD_STOP, or SD_UNLOAD_TASKS for a late unload, the
// stop hook having been called once). With free_now the image goes at the end (state TILE); without it the caller frees it
// later (a card's removal, after the screens are closed). False: a step failed, the image stays and the app is FAULTED
// (resumed by late_arm when it was the stop hook or the tasks); the leak is logged.
static bool unload(sd_app_rec_t *rec, sd_unload_step_t from, bool free_now) {
    sd_unload_step_t step = from;
    while (step != SD_UNLOAD_FREE && step != SD_UNLOAD_FAULT) {
        bool ok = unload_step(rec, step);
        if (!ok) {
            ESP_LOGE(TAG, "%s: unload failed at step %d (0 stop hook, 1 tasks, 2 timers/UI): image and tile kept", rec->m.id, (int)step);
            sd_app_lock();
            sd_app_unload_fault(&rec->sda, step);
            bool late = rec->sda.late;
            rec->fault_ms = lichen_sys_life_now_ms();
            sd_app_unlock();
            if (late) {
                atomic_fetch_add(&g_late_apps, 1);
            }
        }
        step = sd_unload_next(step, ok);
    }
    if (step == SD_UNLOAD_FAULT) {
        late_arm(rec);
        return false;
    }
    if (free_now && !release_app_holdings(rec)) {
        sd_app_lock();
        sd_app_unload_done(&rec->sda, false);
        sd_app_unlock();
        return false;
    }
    if (free_now) {
        free_image(rec);
        sd_app_lock();
        sd_app_unload_done(&rec->sda, true);
        sd_app_unlock();
    }
    return true;
}

// ---- load ------------------------------------------------------------------------------------------------------------

static void fail_load(sd_app_rec_t *rec, char const *why) {
    ESP_LOGW(TAG, "%s: %s", rec->m.id, why);
    snprintf(rec->msg, sizeof(rec->msg), "%s", why);
    lichen_sys_notify(LN_WARN, rec->m.name, why, 0);
    lichen_sys_ui_call(show_msg_cb, rec, UI_WAIT_MS);
}

typedef struct {
    size_t psram, internal;
} heap_snap_t;

static heap_snap_t heap_now(void) {
    return (heap_snap_t){heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_INTERNAL)};
}

// Reads and relocates the ELF. A plain failure text, or NULL when the image is held
static char const *read_image(sd_app_rec_t *rec) {
    char path[sizeof(SD_APPS_DIR) + LICHEN_MANIFEST_ID_MAX + LICHEN_MANIFEST_NAME_MAX + 4];
    snprintf(path, sizeof(path), "%s/%s/%s", SD_APPS_DIR, rec->m.id, rec->m.entry);
    if (!sd_begin(SD_WAIT_MS)) {
        return "The card is busy or gone";
    }
    sd_elf_file_t f;
    if (!sd_elf_open_source(path, &f)) {
        sd_end(sd_io_from_errno(errno));
        return "The program file cannot be read";
    }
    rec->prog = (sd_progress_t){0};
    f.on_read = read_progress;
    f.arg     = rec;
    elf_source_t src = sd_elf_source(&f);
    esp_elf_init(&rec->elf);
    lichen_ctx_t prev = lichen_ctx_enter(rec->ctx); // the image is booked under the app
    int ret = esp_elf_relocate_source(&rec->elf, &src); // closes the file
    lichen_ctx_leave(prev);
    sd_end(ret == 0 ? SD_IO_OK : SD_IO_NEUTRAL);        // a bad image says nothing about the card
    if (ret != 0) {
        esp_elf_deinit(&rec->elf);
        ESP_LOGW(TAG, "%s: relocate failed (%d)", rec->m.id, ret);
        return "The program could not be loaded";
    }
    rec->image = true;
    sd_app_lock();
    rec->pct = 100; // relocation, main() and the hooks follow: a full bar and "Starting..."
    sd_app_unlock();
    lichen_sys_ui_call(progress_cb, rec, UI_WAIT_MS);
    return NULL;
}

static void app_main_task(void *arg) {
    sd_app_rec_t *rec = arg;
    // main() is called directly, like the spike: the ELF's init_array does not run, so an app has no C constructors
    if (rec->elf.entry) {
        rec->elf.entry(0, NULL);
    }
    xSemaphoreGive(g_hooks_sem); // wakes a loader still waiting for hooks that never came
}

static bool start_main(sd_app_rec_t *rec) {
    xSemaphoreTake(g_hooks_sem, 0);
    lichen_sys_timer_revive(rec->ctx); // an earlier unload killed the app's timers
    sd_app_lock();
    rec->stop_called = false;
    sd_app_unlock();
    // An internal stack, not PSRAM: a PSRAM stack must never return and never touch flash, NVS or /storage (task_book.h),
    // and an app's main() returns and may use settings and files
    // The OS starts main() for the app (no BACKGROUND needed: it is the app's entry, not a task the app asked for); a task
    // the app starts itself needs the cap in its manifest. Booked under the app, so the unload waits for it
    return lichen_os_task_start_for(rec->ctx, "sdapp", app_main_task, rec, stack_bytes_of(rec), LICHEN_PRIO_BACKGROUND,
                                    LICHEN_STACK_INTERNAL, NULL);
}

// After the load: the hooks are set, or why not. True when the app is ready for open
static char const *run_main(sd_app_rec_t *rec) {
    if (!start_main(rec)) {
        return "No task for the app";
    }
    bool got = xSemaphoreTake(g_hooks_sem, pdMS_TO_TICKS(HOOKS_WAIT_MS)) == pdTRUE;
    sd_app_lock();
    bool set = rec->hooks_set;
    sd_app_unlock();
    if (!set) {
        return got ? "The app set no hooks" : "The app did not start";
    }
    return NULL;
}

// Everything after the image is held went wrong: the app goes through the unload like any other
static void abort_after_image(sd_app_rec_t *rec, char const *why) {
    sd_app_lock();
    sd_app_load_abort(&rec->sda); // LOADING to UNLOADING: the Loading screen that waits is the OS's
    sd_app_unlock();
    unload(rec, SD_UNLOAD_STOP, true);
    fail_load(rec, why);
}

static void do_load(sd_app_rec_t *rec) {
    vTaskDelay(pdMS_TO_TICKS(OPEN_SETTLE_MS));
    char const *hold = console_usb_hold_why(); // checked again right before the read: the hold may have come on since
    sd_app_lock();
    bool skip = sd_app_load_skip(&rec->sda); // Esc on the Loading screen closed it: read nothing (#546)
    bool go   = rec->sda.state == SD_APP_LOADING && !rec->sda.removing;
    sd_app_unlock();
    if (skip) {
        ESP_LOGI(TAG, "%s: closed before the load", rec->m.id);
        return;
    }
    char const *why = !go ? "The card is going" : hold ? "Not while the on-air hold is on" : NULL;
    if (!why) {
        heap_snap_t before = heap_now();
        why                = read_image(rec);
        if (!why) {
            heap_snap_t loaded = heap_now();
            why                = run_main(rec);
            ESP_LOGI(TAG, "%s: loaded %d B PSRAM %d B int", rec->m.id,
                     (int)(before.psram - loaded.psram), (int)(before.internal - loaded.internal));
            if (why) {
                abort_after_image(rec, why);
                return;
            }
        }
    }
    sd_app_lock();
    sd_next_t next = sd_app_load_done(&rec->sda, why == NULL);
    sd_app_unlock();
    if (why) {
        if (go) {
            fail_load(rec, why);
        }
        return;
    }
    if (next == SD_NEXT_OPEN) {
        if (!lichen_sys_ui_call(open_cb, rec, UI_WAIT_MS)) {
            fail_load(rec, "Screen busy: reopen the app");
        }
    } else if (next == SD_NEXT_UNLOAD) {
        post(MSG_CLOSED, rec); // closed while it loaded: the usual road
    }
}

#define RETRY_MS  250 // a screen the app pushed itself is still on the stack: ask again, without holding the loader
#define RETRY_MAX 80  // 20 s, then the app stays loaded (logged)

// Whether the shell's stack still holds a screen of the app. An answer that is not there (the ctx left) is unknown and counts
// as "yes": the removal path owns an app that left
static bool app_shows_screen(sd_app_rec_t const *rec) {
    app_life_t life;
    return !lichen_sys_life_get(rec->ctx, &life) || life.state == LIFE_OPEN || life.state == LIFE_SUSPENDED;
}

// Apps whose close is waiting for their own screens to go: by ctx, never by a kept record pointer (the record may be freed
// meanwhile; sd_app_find answers NULL then)
typedef struct {
    lichen_ctx_t ctx;
    uint8_t      tries;
} retry_t;
static retry_t g_retry[SD_APPS_MAX];

static bool retry_pending(void) {
    for (size_t i = 0; i < SD_APPS_MAX; i++) {
        if (g_retry[i].ctx) {
            return true;
        }
    }
    return false;
}

static void retry_add(lichen_ctx_t ctx) {
    size_t slot = SD_APPS_MAX;
    for (size_t i = 0; i < SD_APPS_MAX; i++) {
        if (g_retry[i].ctx == ctx) {
            return;
        }
        if (!g_retry[i].ctx && slot == SD_APPS_MAX) {
            slot = i;
        }
    }
    if (slot == SD_APPS_MAX) {
        ESP_LOGE(TAG, "close queue full");
        return;
    }
    g_retry[slot] = (retry_t){ctx, 0};
}

// True when the close is settled (unloaded, or opened again, or nothing left to do); false: a screen of the app is still shown
static bool closed_try(sd_app_rec_t *rec) {
    // The last counted screen's delete event started this: the barrier call queues behind that deletion, so the widgets are
    // gone. Screens the app pushed itself are not counted: the shell's lifecycle says whether any is on the stack. An SD
    // app's widgets live on its own screens (docs, #524): nothing on a layer above them is watched.
    ui_barrier(LICHEN_CTX_NONE);
    if (app_shows_screen(rec)) {
        return false;
    }
    sd_app_lock();
    bool begin = sd_app_unload_begin(&rec->sda);
    // A removal starts the unload with a counted screen still animating out: then the image stays, and the free is left to
    // remove_one and the free_pending / MSG_FREE path (sd_app_may_free)
    bool free_now = sd_app_may_free(&rec->sda);
    sd_app_unlock();
    if (!begin) {
        return true; // opened again
    }
    heap_snap_t before = heap_now();
    if (unload(rec, SD_UNLOAD_STOP, free_now) && free_now) {
        heap_snap_t after = heap_now();
        ESP_LOGI(TAG, "%s: unloaded %+d B %+d B", rec->m.id, (int)(after.psram - before.psram),
                 (int)(after.internal - before.internal));
    }
    return true;
}

static void do_closed(sd_app_rec_t *rec) {
    if (!closed_try(rec)) {
        retry_add(rec->ctx);
    }
}

// The loader finishes an unload it gave up on, now that no task of the app is left (#541). The app is FAULTED: opens were
// refused meanwhile, and the state is UNLOADING for the rest, so a reopen is told to wait. Nothing is freed unless
// sd_app_late_begin saw no task and no screen; the same steps run as after a normal close (timers and UI, holdings, image).
static void do_late(lichen_ctx_t ctx) {
    sd_app_rec_t *rec = sd_app_find(ctx);
    if (!rec) {
        return;
    }
    size_t left = lichen_os_task_count(ctx);
    sd_app_lock();
    bool begin    = sd_app_late_begin(&rec->sda, left);
    bool removing = rec->sda.removing;
    sd_app_unlock();
    if (!begin) {
        return; // a task is back (its end asks again), a screen is left (its delete asks again), or an earlier message did it
    }
    atomic_fetch_sub(&g_late_apps, 1);
    heap_snap_t before = heap_now();
    if (unload(rec, SD_UNLOAD_TASKS, true)) {
        heap_snap_t after = heap_now();
        ESP_LOGI(TAG, "%s: unloaded late, its last task has ended: %+d B %+d B", rec->m.id,
                 (int)(after.psram - before.psram), (int)(after.internal - before.internal));
        if (removing) {
            post(MSG_CARD, NULL); // the card went while it ran: the removal that gave up can finish now
        }
    }
}

static void retry_run(void) {
    for (size_t i = 0; i < SD_APPS_MAX; i++) {
        if (!g_retry[i].ctx) {
            continue;
        }
        sd_app_rec_t *rec = sd_app_find(g_retry[i].ctx);
        if (!rec || closed_try(rec)) {
            g_retry[i].ctx = LICHEN_CTX_NONE; // settled, or the app left
        } else if (++g_retry[i].tries >= RETRY_MAX) {
            ESP_LOGW(TAG, "%s: still shows a screen, stays loaded", rec->m.id);
            g_retry[i].ctx = LICHEN_CTX_NONE;
        }
    }
}

// ---- removal ---------------------------------------------------------------------------------------------------------

static void freed_cb(void *arg) { // lichen_unregister_app's done, on the LVGL task: the screens are closed
    if (!post(MSG_FREE, arg)) {
        ESP_LOGE(TAG, "%s: free not queued", ((sd_app_rec_t *)arg)->m.id);
    }
}

static void unregister_one(sd_app_rec_t *rec) {
    if (!lichen_sys_life_removable(rec->ctx) && lichen_sys_life_stop_begin(rec->ctx, LIFE_STOP_UNLOAD) == LIFE_BEGIN_OK) {
        lichen_sys_life_stop_end(rec->ctx, true, (uint32_t)lichen_task_leftovers(rec->ctx, 0));
    }
    // Marked first: from here on no close callback finds the record, so no message about it is queued behind the free
    sd_app_lock();
    rec->gone = true;
    sd_app_unlock();
    uint32_t       leaked = 0;
    lichen_unreg_t r      = lichen_unregister_app(rec->ctx, &leaked, freed_cb, rec);
    for (int i = 0; i < UNREG_TRIES && (r == LICHEN_UNREG_BUSY || r == LICHEN_UNREG_UNNOTIFIED); i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
        r = r == LICHEN_UNREG_BUSY ? lichen_unregister_app(rec->ctx, &leaked, freed_cb, rec)
                                   : lichen_app_list_renotify(freed_cb, rec);
    }
    if (leaked) {
        ESP_LOGW(TAG, "%s: %u blocks leaked", rec->m.id, (unsigned)leaked);
    }
    if (r != LICHEN_UNREG_OK && r != LICHEN_UNREG_PENDING) {
        ESP_LOGE(TAG, "%s: unregister %d", rec->m.id, (int)r);
        sd_app_lock();
        rec->gone = false; // found again by the next remove_all; its removing flag stays, so it opens nothing
        sd_app_unlock();
    }
}

static void remove_one(sd_app_rec_t *rec) {
    sd_app_lock();
    sd_app_remove(&rec->sda);
    bool begin = sd_app_unload_begin(&rec->sda);
    bool bad   = rec->sda.state == SD_APP_FAULTED;
    sd_app_unlock();
    if (begin) {
        bad = !unload(rec, SD_UNLOAD_STOP, false); // the image is freed after the screens are closed (do_free)
    }
    bad = !release_app_holdings(rec) || bad; // the card is gone or changed: files and raw keys go while the context is still valid
    sd_app_lock();
    bool may = sd_app_may_unregister(&rec->sda, 0);
    sd_app_unlock();
    size_t left = lichen_os_task_count(rec->ctx);
    if (bad || !may || left) {
        // Its image, a task or a timer may still run: it stays registered and FAULTED until the badge restarts, with its
        // tile (opening it is refused) and its record; nothing of it is freed under what may still run
        ESP_LOGE(TAG, "%s: not removed, %u tasks left", rec->m.id, (unsigned)left);
        sd_app_lock();
        if (rec->sda.state == SD_APP_UNLOADING) {
            sd_app_unload_done(&rec->sda, false);
        }
        sd_app_unlock();
        return;
    }
    unregister_one(rec);
}

static bool rec_listed(sd_app_rec_t const *rec) {
    sd_app_lock();
    sd_app_rec_t const *r = g_recs;
    while (r && r != rec) {
        r = r->next;
    }
    bool gone = r && r->gone; // a new record at a freed one's address is not gone: never freed by a stale message
    sd_app_unlock();
    return gone;
}

static void do_free(sd_app_rec_t *rec) {
    if (!rec_listed(rec)) {
        return; // freed by an earlier message: one unregister frees once, whoever posted
    }
    // unregister's done only says the shell was told to close the screens: one that is still animating out has its delete
    // event (and ELF callbacks) to come, so the last of them posts the free again
    sd_app_lock();
    bool wait = !sd_app_may_free(&rec->sda);
    rec->free_pending = wait;
    sd_app_unlock();
    if (wait) {
        return;
    }
    ui_barrier(LICHEN_CTX_NONE); // the deletions are complete, and whatever LVGL still had queued for the app has run
    lichen_sys_ui_cancel(progress_cb, rec); // a call the full queue handed to lv_async_call may have passed the barrier
    // A faulted app is never unregistered (remove_one), so this one stopped cleanly
    if (!release_app_holdings(rec)) {
        return; // image and record stay: nothing is freed under what may still be called
    }
    if (rec->image) {
        free_image(rec);
    }
    unlink_rec(rec);
    vSemaphoreDelete(rec->stop_sem);
    heap_caps_free(rec);
}

// One pass: an app that could not leave (gone cleared again) is skipped here and tried by the next card change
static void remove_all(void) {
    for (size_t skip = 0;; ) {
        sd_app_rec_t *rec = NULL;
        size_t        seen = 0;
        sd_app_lock();
        for (sd_app_rec_t *r = g_recs; r; r = r->next) {
            if (!r->gone && seen++ == skip) {
                rec = r;
                break;
            }
        }
        sd_app_unlock();
        if (!rec) {
            return;
        }
        remove_one(rec);
        sd_app_lock();
        skip += rec->gone ? 0 : 1; // gone: the next one is at the same place
        sd_app_unlock();
    }
}

static void do_card(void) {
    remove_all(); // a card that went, or one that came back: the apps of the old mount leave first
    if (sd_mounted()) {
        sd_app_scan();
    }
}

static void loader_task(void *arg) {
    (void)arg;
    for (;;) {
        msg_t m;
        bool again = retry_pending() || atomic_load(&g_late_retry);
        if (xQueueReceive(g_queue, &m, again ? pdMS_TO_TICKS(RETRY_MS) : portMAX_DELAY) != pdTRUE) {
            retry_run();
            if (atomic_load(&g_late_retry)) {
                late_sweep();
            }
            continue;
        }
        switch (m.kind) {
        case MSG_CARD:
            do_card();
            break;
        case MSG_LOAD:
            do_load(m.rec);
            break;
        case MSG_CLOSED:
            do_closed(m.rec);
            break;
        case MSG_FREE:
            do_free(m.rec);
            break;
        case MSG_LATE:
            do_late(m.ctx);
            break;
        }
    }
}

// ---- boot --------------------------------------------------------------------------------------------------------------

void sd_apps_register_symbols(void) {
    static atomic_bool done; // a second esp_elf_register_symbol would only answer -EEXIST
    if (!atomic_exchange(&done, true)) {
        esp_elf_register_symbol(lichen_exports);
    }
}

// The card changed, any way (a mount and a format too: the old apps are removed and the card is scanned again): marks every
// app's removal at once (an open stops being accepted) and tells the loader
static void card_changed(sd_state_t state) {
    (void)state;
    sd_app_lock();
    for (sd_app_rec_t *r = g_recs; r; r = r->next) {
        r->sda.removing = true;
    }
    sd_app_unlock();
    if (!post(MSG_CARD, NULL)) {
        ESP_LOGE(TAG, "card event lost");
    }
}

void sd_apps_boot(void) {
    if (atomic_exchange(&g_started, true)) {
        return;
    }
    if (safe_mode_degraded_boot()) {
        ESP_LOGW(TAG, "no SD apps: crash in the last boots");
        return;
    }
    sd_apps_register_symbols();
    g_mu        = xSemaphoreCreateMutex();
    g_hooks_sem = xSemaphoreCreateBinary();
    g_ui_sem    = xSemaphoreCreateBinary();
    g_blk_mu    = xSemaphoreCreateMutex();
    g_queue     = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    if (!g_mu || !g_blk_mu || !g_hooks_sem || !g_ui_sem || !g_queue) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    if (!lichen_task_start(lichen_sys_ctx_os(), "sdapps", loader_task, NULL, LOADER_STACK, LICHEN_PRIO_SERVICE, NULL)) {
        ESP_LOGE(TAG, "loader task failed");
        return;
    }
    lichen_os_task_end_notify(task_ended);
    sd_observe(card_changed);
    if (sd_mounted()) { // no event for a card that mounted before the observer
        post(MSG_CARD, NULL);
    }
}

// ---- exit ------------------------------------------------------------------------------------------------------------

// What an SD app's exit() resolves to (tools/export_allow.txt, #533): ask the OS to close the app, then end this task like a
// return would. The normal unload runs from there (stop hook, tasks, timers, barrier); nothing is freed under this task.
// Only this app's screens go, and only when the app is on top (the user may be elsewhere): the screens it pushed and its own,
// not the whole stack. The call never returns, because the app's compiler knows it as noreturn: a booked task ends; any
// other task (the LVGL task, a timer or console task, a context that is no SD app) cannot end, so on the LVGL task, which
// cannot be parked without freezing the display, the badge aborts with a log line naming the cause (the crash is attributed to
// the app, like any other fault); any other unbooked task (or a context without an app) is suspended for good after the
// close was asked. A PSRAM-stack task is parked by lichen_os_task_end_self (the unload then faults), and
// exit() from the stop hook ends its task before it answers (the unload times out and faults).
// One pop job to the screen under the app's root: the shell pops down to it only if it is still on the stack and runs no follow-up,
// so an Esc between two steps cannot take another app's screen. Nothing when the root is not on the stack.
static void close_app_screens(sd_app_rec_t *rec) {
    app_life_t life;
    if (!lichen_sys_life_get(rec->ctx, &life) || life.state != LIFE_OPEN) {
        return; // another screen covers the app: the user stays where they are, the unload follows when the app's screens go
    }
    sd_app_lock();
    lv_obj_t *root = rec->screen;
    sd_app_unlock();
    for (int i = 0; root && i < LIFE_SCREENS_MAX; i++) {
        lv_obj_t *at = shell_stack_screen(i);
        if (!at) {
            return;
        }
        if (at == root) {
            lv_obj_t *below = shell_stack_screen(i + 1);
            if (below) {
                shell_back_to(below, NULL, NULL);
            }
            return;
        }
    }
}

// exit() on a task that cannot end: on the LVGL task (parking it would freeze the display) the badge aborts, which is
// attributed to the app; any other task is suspended for good. Never returns.
__attribute__((noreturn)) static void park_caller(char const *id, lichen_ctx_t ctx) {
    lichen_fs_sd_release_ctx(ctx); // the trampoline's release never runs for a task that stays: a batch it left open locks the card
    if (lichen_ui_ops_lock_held()) {
        ESP_LOGE(TAG, "%s: exit() on the LVGL task cannot end it: abort", id ? id : "(no app)");
        abort();
    }
    ESP_LOGW(TAG, "%s: exit() on a task the OS did not start: suspended for good", id ? id : "(no app)");
    for (;;) {
        vTaskSuspend(NULL);
    }
}

void lichen_exp_exit(int code) {
    sd_app_rec_t *rec = sd_app_find(lichen_ctx_self());
    if (!rec) {
        ESP_LOGE(TAG, "exit(%d) from a context that is no SD app: nothing to close", code);
        park_caller(NULL, lichen_ctx_self());
    }
    sd_app_lock();
    unsigned todo = sd_app_exit(&rec->sda);
    sd_app_unlock();
    ESP_LOGI(TAG, "%s: exit(%d)", rec->m.id, code);
        if (todo & SD_EXIT_WAKE) {
        xSemaphoreGive(g_hooks_sem); // the loader's wait for hooks main() will not give
    }
    if (todo & SD_EXIT_HOME) {
        close_app_screens(rec);
    }
    if ((todo & SD_EXIT_UNLOAD) && !post(MSG_CLOSED, rec)) {
        ESP_LOGE(TAG, "%s: queue full, stays loaded", rec->m.id);
    }
    if (lichen_os_task_end_self()) {
        return; // not reached
    }
    park_caller(rec->m.id, rec->ctx);
}
