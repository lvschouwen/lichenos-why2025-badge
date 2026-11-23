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

#include "lichen_exports.h"
#include "lichen_sys_app.h"
#include "lichen_sys_life.h"
#include "lichen_sys_notify.h"
#include "lichen_sys_ui.h"
#include "lichen_task.h"
#include "sd_app_svc.h"
#include "sd_elf_src.h"
#include "sdcard.h"

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fw_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "safe_mode.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "sdapps";

#define LOADER_STACK       6144 // the loader's own: manifests and paths on it, no big buffers
#define OPEN_SETTLE_MS     450  // the ELF loads after the open animation: its cache flush is the #31 window
#define CLOSE_SETTLE_MS    700  // a closed screen is deleted at the end of its animation; reopened within it, it stays
#define HOOKS_WAIT_MS      5000 // main() has this long to call lichen_sd_app_hooks
#define TASKS_WAIT_MS      3000 // after the stop hook, the app's tasks have this long to return
#define UI_WAIT_MS         3000 // the barrier call through the LVGL task
#define SD_WAIT_MS         2000
#define UNREG_TRIES        20
#define STACK_KIB_DEFAULT  4
#define STACK_KIB_MAX      32

typedef enum { MSG_CARD, MSG_LOAD, MSG_CLOSED, MSG_FREE } msg_kind_t;
typedef struct {
    msg_kind_t    kind;
    sd_app_rec_t *rec;
} msg_t;

static SemaphoreHandle_t g_mu;
static SemaphoreHandle_t g_hooks_sem; // main() gave its hooks, or returned
static SemaphoreHandle_t g_ui_sem;    // the barrier call ran
static QueueHandle_t     g_queue;
static sd_app_rec_t     *g_recs; // the loader task adds and removes; others read under g_mu
static _Atomic bool      g_started;

void sd_app_lock(void) {
    xSemaphoreTake(g_mu, portMAX_DELAY);
}
void sd_app_unlock(void) {
    xSemaphoreGive(g_mu);
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
    msg_t m = {kind, rec};
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

// The screen waiting for an app goes away (closed, or the shell rebuilt Home): the record forgets it
static void screen_deleted(lv_event_t *e) {
    sd_app_rec_t *rec = lv_event_get_user_data(e);
    sd_app_lock();
    if (rec->screen == lv_event_get_target(e)) {
        rec->screen = NULL;
    }
    sd_app_unlock();
}

static void refused_say(sd_app_rec_t *rec, char const *why) {
    ESP_LOGW(TAG, "%s: open refused: %s", rec->m.id, why);
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
    sd_open_t v = sd_app_open(&rec->sda, hold);
    if (v == SD_OPEN_LOAD || v == SD_OPEN_WAIT) {
        rec->screen = screen;
    } else if (v == SD_OPEN_NOW) {
        h = rec->hooks;
    }
    sd_app_unlock();
    switch (v) {
    case SD_OPEN_NOW:
        h.open(screen); // the shell entered the app's context
        return;
    case SD_OPEN_LOAD:
    case SD_OPEN_WAIT:
        say(screen, "Loading...");
        lv_obj_add_event_cb(screen, screen_deleted, LV_EVENT_DELETE, rec);
        if (v == SD_OPEN_LOAD && !post(MSG_LOAD, rec)) {
            sd_app_lock();
            sd_app_load_done(&rec->sda, false);
            sd_app_unlock();
            say(screen, "The loader is busy: try again");
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

// An update is coming: the app's stop hook, if it is loaded and not stopped yet. The OTA task; the image is not unloaded
// (the badge restarts). The unload waits for a caller still inside the hook before it frees the image.
bool sd_app_tramp_stop(void) {
    sd_app_rec_t *rec = sd_app_find(lichen_ctx_self());
    if (!rec) {
        return true;
    }
    lichen_sd_hooks_t h = {0};
    sd_app_lock();
    bool call = sd_app_hooks_live(&rec->sda) && !rec->stop_called && rec->hooks.stop;
    if (call) {
        h                = rec->hooks;
        rec->stop_called = true;
        rec->stop_inflight++;
    }
    sd_app_unlock();
    if (!call) {
        return true; // not loaded, no hook, or already stopped
    }
    bool ok = h.stop();
    sd_app_lock();
    rec->stop_inflight--;
    sd_app_unlock();
    return ok;
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
    return xSemaphoreTake(g_ui_sem, pdMS_TO_TICKS(UI_WAIT_MS)) == pdTRUE;
}

// The hook is called once per load (an update's stop may have called it already), and the image is freed only after every
// caller left it: the wait is part of the tasks step
static bool run_stop_hook(sd_app_rec_t *rec) {
    lichen_sd_hooks_t h = {0};
    sd_app_lock();
    bool call = rec->hooks_set && !rec->stop_called && rec->hooks.stop;
    if (call) {
        h                = rec->hooks;
        rec->stop_called = true;
        rec->stop_inflight++;
    }
    sd_app_unlock();
    if (!call) {
        return true; // nothing to stop but main() returning, or stopped already
    }
    uint32_t     t0   = lichen_sys_life_now_ms();
    lichen_ctx_t prev = lichen_ctx_enter(rec->ctx);
    bool         ok   = h.stop();
    lichen_ctx_leave(prev);
    sd_app_lock();
    rec->stop_inflight--;
    sd_app_unlock();
    uint32_t took = lichen_sys_life_now_ms() - t0;
    if (ok && took > app_life_deadline_ms(LIFE_STOP_UNLOAD)) {
        ESP_LOGE(TAG, "%s: the stop hook took %u ms, past its deadline", rec->m.id, (unsigned)took);
        ok = false; // late: reported, never interrupted; the image stays
    }
    return ok;
}

// Nobody is inside the stop hook any more (an update's stop on the OTA task may still be)
static bool stop_hook_left(sd_app_rec_t *rec) {
    for (uint32_t waited = 0; waited <= TASKS_WAIT_MS; waited += 20) {
        sd_app_lock();
        int n = rec->stop_inflight;
        sd_app_unlock();
        if (!n) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool unload_step(sd_app_rec_t *rec, sd_unload_step_t step) {
    switch (step) {
    case SD_UNLOAD_STOP:
        return run_stop_hook(rec);
    case SD_UNLOAD_TASKS:
        return stop_hook_left(rec) && lichen_task_leftovers(rec->ctx, TASKS_WAIT_MS) == 0;
    case SD_UNLOAD_UI:
        return ui_barrier(rec->ctx);
    default:
        return true;
    }
}

static void free_image(sd_app_rec_t *rec) {
    esp_elf_deinit(&rec->elf);
    memset(&rec->elf, 0, sizeof(rec->elf));
    sd_app_lock();
    rec->image     = false;
    rec->hooks_set = false;
    sd_app_unlock();
}

// The app's state is UNLOADING: the steps in order. With free_now the image goes at the end (state TILE); without it the
// caller frees it later (a card's removal, after the screens are closed). False: a step failed, the image stays and the
// app is FAULTED; the leak is logged.
static bool unload(sd_app_rec_t *rec, bool free_now) {
    sd_unload_step_t step = SD_UNLOAD_STOP;
    while (step != SD_UNLOAD_FREE && step != SD_UNLOAD_FAULT) {
        bool ok = unload_step(rec, step);
        if (!ok) {
            ESP_LOGE(TAG, "%s: unload failed at the %s: the image stays", rec->m.id, sd_unload_step_name(step));
        }
        step = sd_unload_next(step, ok);
    }
    if (step == SD_UNLOAD_FAULT) {
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
    elf_source_t src = sd_elf_source(&f);
    esp_elf_init(&rec->elf);
    int ret = esp_elf_relocate_source(&rec->elf, &src); // closes the file
    sd_end(ret == 0 ? SD_IO_OK : SD_IO_NEUTRAL);        // a bad image says nothing about the card
    if (ret != 0) {
        esp_elf_deinit(&rec->elf);
        ESP_LOGW(TAG, "%s: relocate failed (%d)", rec->m.id, ret);
        return "The program could not be loaded (it needs something this OS does not have)";
    }
    rec->image = true;
    return NULL;
}

static void app_main_task(void *arg) {
    sd_app_rec_t *rec = arg;
    int           ret = rec->elf.entry ? rec->elf.entry(0, NULL) : -1;
    ESP_LOGI(TAG, "%s: main() returned %d", rec->m.id, ret);
    xSemaphoreGive(g_hooks_sem); // wakes a loader still waiting for hooks that never came
}

static bool start_main(sd_app_rec_t *rec) {
    uint32_t kib = rec->m.stack_kib ? rec->m.stack_kib : STACK_KIB_DEFAULT;
    if (kib > STACK_KIB_MAX) {
        kib = STACK_KIB_MAX;
    }
    xSemaphoreTake(g_hooks_sem, 0);
    lichen_sys_timer_revive(rec->ctx); // an earlier unload killed the app's timers
    sd_app_lock();
    rec->stop_called = false;
    sd_app_unlock();
    // An internal stack, not PSRAM: a PSRAM stack must never return and never touch flash, NVS or /storage (task_book.h),
    // and an app's main() returns and may use settings and files
    return lichen_task_start(rec->ctx, "sdapp", app_main_task, rec, kib * 1024u, LICHEN_PRIO_BACKGROUND, NULL);
}

// After the load: the hooks are set, or why not. True when the app is ready for open
static char const *run_main(sd_app_rec_t *rec) {
    if (!start_main(rec)) {
        return "No task for the app (memory or the task book)";
    }
    bool got = xSemaphoreTake(g_hooks_sem, pdMS_TO_TICKS(HOOKS_WAIT_MS)) == pdTRUE;
    sd_app_lock();
    bool set = rec->hooks_set;
    sd_app_unlock();
    if (!set) {
        return got ? "The app set no hooks" : "The app did not start in time";
    }
    return NULL;
}

// Everything after the image is held went wrong: the app goes through the unload like any other
static void abort_after_image(sd_app_rec_t *rec, char const *why) {
    sd_app_lock();
    sd_app_load_done(&rec->sda, true); // LOADED, so the unload may begin
    rec->sda.closing = true;
    sd_app_unload_begin(&rec->sda);
    sd_app_unlock();
    unload(rec, true);
    fail_load(rec, why);
}

static void do_load(sd_app_rec_t *rec) {
    vTaskDelay(pdMS_TO_TICKS(OPEN_SETTLE_MS));
    char const *hold = console_usb_hold_why(); // checked again right before the read: the hold may have come on since
    sd_app_lock();
    bool go = rec->sda.state == SD_APP_LOADING && !rec->sda.removing;
    sd_app_unlock();
    char const *why = !go ? "The card is going" : hold ? "Not while the on-air hold is on" : NULL;
    if (!why) {
        heap_snap_t before = heap_now();
        why                = read_image(rec);
        if (!why) {
            heap_snap_t loaded = heap_now();
            why                = run_main(rec);
            ESP_LOGI(TAG, "%s: loaded, image %d B PSRAM, %d B internal (read before open())", rec->m.id,
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
            ESP_LOGW(TAG, "%s: the UI was busy: no open", rec->m.id);
        }
    } else if (next == SD_NEXT_UNLOAD) {
        post(MSG_CLOSED, rec); // closed while it loaded: the usual road
    }
}

static void do_closed(sd_app_rec_t *rec) {
    vTaskDelay(pdMS_TO_TICKS(CLOSE_SETTLE_MS));
    ui_barrier(LICHEN_CTX_NONE); // the screen's deletion, if the shell queued it, has run
    sd_app_lock();
    bool begin = sd_app_unload_begin(&rec->sda);
    sd_app_unlock();
    if (!begin) {
        return; // opened again within the settle
    }
    heap_snap_t before = heap_now();
    if (unload(rec, true)) {
        heap_snap_t after = heap_now();
        ESP_LOGI(TAG, "%s: unloaded, PSRAM %+d B, internal %+d B", rec->m.id, (int)(after.psram - before.psram),
                 (int)(after.internal - before.internal));
    }
}

// The app's last screen left the stack (lichen_sys_life_on_close; the LVGL task, no waiting)
static void sd_app_on_close(lichen_ctx_t ctx) {
    sd_app_rec_t *rec = sd_app_find(ctx);
    if (!rec) {
        return;
    }
    sd_app_lock();
    bool plan = sd_app_closed(&rec->sda);
    sd_app_unlock();
    if (plan) {
        post(MSG_CLOSED, rec);
    }
}

// ---- removal ---------------------------------------------------------------------------------------------------------

static void freed_cb(void *arg) { // lichen_unregister_app's done, on the LVGL task: the screens are closed
    if (!post(MSG_FREE, arg)) {
        ESP_LOGE(TAG, "%s: no room to free the app's record: left as it is", ((sd_app_rec_t *)arg)->m.id);
    }
}

static void unregister_one(sd_app_rec_t *rec, bool faulted) {
    if (!lichen_sys_life_removable(rec->ctx) && lichen_sys_life_stop_begin(rec->ctx, LIFE_STOP_UNLOAD) == LIFE_BEGIN_OK) {
        lichen_sys_life_stop_end(rec->ctx, !faulted, (uint32_t)lichen_task_leftovers(rec->ctx, 0));
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
        ESP_LOGW(TAG, "%s: left %u lichen_alloc blocks", rec->m.id, (unsigned)leaked);
    }
    if (r != LICHEN_UNREG_OK && r != LICHEN_UNREG_PENDING) {
        ESP_LOGE(TAG, "%s: not unregistered (%d): the tile stays until the next card change", rec->m.id, (int)r);
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
        bad = !unload(rec, false); // the image is freed after the screens are closed (do_free)
    }
    unregister_one(rec, bad);
}

static void do_free(sd_app_rec_t *rec) {
    ui_barrier(LICHEN_CTX_NONE); // whatever LVGL still had queued for the app's screens has run
    sd_app_lock();
    sd_app_state_t st = rec->sda.state;
    sd_app_unlock();
    if (st == SD_APP_FAULTED) {
        ESP_LOGE(TAG, "%s: the image stays loaded and its record is not freed (the app did not stop)", rec->m.id);
        return;
    }
    heap_snap_t before = heap_now();
    if (rec->image) {
        free_image(rec);
    }
    unlink_rec(rec);
    heap_caps_free(rec);
    heap_snap_t after = heap_now();
    ESP_LOGI(TAG, "app freed, PSRAM %+d B", (int)(after.psram - before.psram));
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
        if (xQueueReceive(g_queue, &m, portMAX_DELAY) != pdTRUE) {
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

// The card changed: marks every app's removal at once (an open stops being accepted) and tells the loader
static void card_changed(sd_state_t state) {
    if (state != SD_STATE_MOUNTED) {
        sd_app_lock();
        for (sd_app_rec_t *r = g_recs; r; r = r->next) {
            r->sda.removing = true;
        }
        sd_app_unlock();
    }
    if (!post(MSG_CARD, NULL)) {
        ESP_LOGE(TAG, "the loader's queue is full: the card change is not seen until the next one");
    }
}

void sd_apps_boot(void) {
    if (atomic_exchange(&g_started, true)) {
        return;
    }
    if (safe_mode_degraded_boot()) {
        ESP_LOGW(TAG, "a crash is among the last boots: no SD apps this boot");
        return;
    }
    sd_apps_register_symbols();
    g_mu        = xSemaphoreCreateMutex();
    g_hooks_sem = xSemaphoreCreateBinary();
    g_ui_sem    = xSemaphoreCreateBinary();
    g_queue     = xQueueCreate(16, sizeof(msg_t));
    if (!g_mu || !g_hooks_sem || !g_ui_sem || !g_queue) {
        ESP_LOGE(TAG, "no memory for the loader");
        return;
    }
    if (!lichen_task_start(lichen_sys_ctx_os(), "sdapps", loader_task, NULL, LOADER_STACK, LICHEN_PRIO_SERVICE, NULL)) {
        ESP_LOGE(TAG, "the loader task did not start");
        return;
    }
    lichen_sys_life_on_close(sd_app_on_close);
    sd_observe(card_changed);
    if (sd_mounted()) { // no event for a card that mounted before the observer
        post(MSG_CARD, NULL);
    }
}
