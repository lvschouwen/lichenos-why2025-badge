// The notification service (#62): lichen_notify.h for apps, notify_os.h for the shell. The history (notify_ring.c,
// pure) lives in PSRAM under one mutex; lichen_notify() writes it from the caller's task and queues a toast for the
// shell's LVGL timer, which only ever reads copies. The file is rewritten whole (tmp, then swapped in, as crash.c does)
// on a short-lived task: 30 s after the last change, 3 s after an error or a clear, so normally a few writes a day.
// Plain FAT writes; nothing maps flash.
#include "notify_os.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fw_console.h"
#include "notify_mute.h"
#include "notify_rate.h"
#include "nvs.h"
#include "storage.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char const TAG[] = "notify";

#define NOTIFY_DIR   STORAGE_BASE "/lichen"
#define NOTIFY_FILE  NOTIFY_DIR "/notify.bin"
#define NOTIFY_OLD   NOTIFY_DIR "/notify.old"
#define NOTIFY_TMP   NOTIFY_DIR "/notify.tmp"
#define SAVE_SLOW_US (30LL * 1000 * 1000)
#define SAVE_SOON_US (3LL * 1000 * 1000)
#define SAVE_STACK   4096
#define TOAST_DEPTH  8
#define ONCE_SLOTS   32
#define DND_KEY      "notify_dnd"

void notify_cmd_register(void); // notify_cmd.c

static SemaphoreHandle_t s_lock;
static nr_ring_t        *s_ring; // PSRAM, under s_lock
static uint8_t          *s_buf;  // PSRAM, the file image; only the save task (or start) uses it
static nrate_t           s_rate; // under s_lock
static nmute_t           s_mute; // under s_lock: the per-source switches read so far
static uint32_t          s_once[ONCE_SLOTS];
static size_t            s_once_n;
static QueueHandle_t     s_toasts;
static esp_timer_handle_t s_save_timer;
static int64_t           s_save_due_us; // when the armed timer fires, 0 when it is not armed; under s_lock
static bool              s_storage;
static atomic_bool       s_started;
static atomic_bool       s_dnd;
static atomic_uint       s_generation;
static bool (*s_clock_set)(void);
static ln_feedback_t     s_feedback;
static portMUX_TYPE      s_save_mux = portMUX_INITIALIZER_UNLOCKED;
static bool              s_saving, s_save_again;
static uint32_t          s_saves, s_save_errors;
static int64_t           s_last_save_ms;

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

// ---- saving ---------------------------------------------------------------------------------------------------------

static bool write_file(char const *path, uint8_t const *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    return (fclose(f) == 0) && ok;
}

// rename does not replace on FAT: the old file steps aside until the new one is in place (crash.c's swap)
static bool swap_in(void) {
    struct stat st;
    bool        had = stat(NOTIFY_FILE, &st) == 0;
    if (had) {
        unlink(NOTIFY_OLD);
        if (rename(NOTIFY_FILE, NOTIFY_OLD) != 0) {
            return false;
        }
    }
    if (rename(NOTIFY_TMP, NOTIFY_FILE) != 0) {
        if (had) {
            rename(NOTIFY_OLD, NOTIFY_FILE);
        }
        return false;
    }
    unlink(NOTIFY_OLD);
    return true;
}

static void schedule_save_locked(int64_t delay_us);

static void save_task(void *arg) {
    (void)arg;
    lock();
    size_t len = nr_encode(s_ring, s_buf, NR_FILE_MAX);
    unlock();
    mkdir(NOTIFY_DIR, 0777); // exists after the first save
    bool ok = len && write_file(NOTIFY_TMP, s_buf, len) && swap_in();
    if (!ok) {
        ESP_LOGW(TAG, "cannot write %s (errno %d)", NOTIFY_FILE, errno);
        unlink(NOTIFY_TMP);
    }
    lock();
    s_saves += ok;
    s_save_errors += !ok;
    s_last_save_ms = esp_timer_get_time() / 1000;
    unlock();
    portENTER_CRITICAL(&s_save_mux);
    s_saving    = false;
    bool again  = s_save_again;
    s_save_again = false;
    portEXIT_CRITICAL(&s_save_mux);
    if (again) {
        lock();
        schedule_save_locked(SAVE_SOON_US);
        unlock();
    }
    vTaskDelete(NULL);
}

// esp_timer task: the write itself runs on its own task, so a slow FAT write never holds up other timers
static void save_due(void *arg) {
    (void)arg;
    lock();
    s_save_due_us = 0;
    unlock();
    portENTER_CRITICAL(&s_save_mux);
    bool busy = s_saving;
    s_saving  = true;
    if (busy) {
        s_save_again = true; // the running save may have read the history before this change
    }
    portEXIT_CRITICAL(&s_save_mux);
    if (busy) {
        return;
    }
    if (xTaskCreatePinnedToCore(save_task, "notify_save", SAVE_STACK, NULL, 2, NULL, 0) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the save task: trying again later");
        portENTER_CRITICAL(&s_save_mux);
        s_saving = false;
        portEXIT_CRITICAL(&s_save_mux);
        lock();
        schedule_save_locked(SAVE_SLOW_US);
        unlock();
    }
}

// Armed for delay_us from now, unless it is already due sooner
static void schedule_save_locked(int64_t delay_us) {
    if (!s_storage || !s_save_timer) {
        return;
    }
    int64_t due = esp_timer_get_time() + delay_us;
    if (s_save_due_us && s_save_due_us <= due) {
        return;
    }
    esp_timer_stop(s_save_timer); // ESP_ERR_INVALID_STATE when it is not running: fine
    if (esp_timer_start_once(s_save_timer, (uint64_t)delay_us) == ESP_OK) {
        s_save_due_us = due;
    }
}

// The history changed: views read it again, the file follows
static void changed_locked(bool soon) {
    atomic_store(&s_generation, s_ring->generation);
    schedule_save_locked(soon ? SAVE_SOON_US : SAVE_SLOW_US);
}

// ---- loading --------------------------------------------------------------------------------------------------------

// The file, its swap leftovers in the order a cut-short swap leaves them; the first whole one wins
static bool load_file(char const *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t len = fread(s_buf, 1, NR_FILE_MAX, f);
    bool   more = fgetc(f) != EOF;
    fclose(f);
    if (!more && nr_decode(s_ring, s_buf, len)) {
        return true;
    }
    ESP_LOGW(TAG, "%s is damaged (%u bytes): not used", path, (unsigned)len);
    return false;
}

static void load(void) {
    char const *paths[] = {NOTIFY_FILE, NOTIFY_OLD, NOTIFY_TMP};
    bool        loaded  = false;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && !loaded; i++) {
        loaded = load_file(paths[i]);
    }
    uint16_t boot = loaded ? s_ring->boot : 0;
    if (!loaded) {
        nr_init(s_ring, 0);
    }
    uint16_t next = (uint16_t)(boot + 1);
    s_ring->boot  = next ? next : 1; // entries of this boot tell themselves apart from the file's
    ESP_LOGI(TAG, "%u notifications kept, boot %u", (unsigned)nr_count(s_ring), (unsigned)s_ring->boot);
}

void lichen_notify_start(bool storage_ok) {
    if (atomic_load(&s_started)) {
        return;
    }
    s_lock   = xSemaphoreCreateMutex();
    s_toasts = xQueueCreate(TOAST_DEPTH, sizeof(ln_toast_t));
    s_ring   = heap_caps_calloc(1, sizeof(*s_ring), MALLOC_CAP_SPIRAM);
    s_buf    = heap_caps_malloc(NR_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_toasts || !s_ring || !s_buf) {
        ESP_LOGE(TAG, "no memory: notifications off");
        return;
    }
    esp_timer_create_args_t const args = {.callback = save_due, .name = "notify_save"};
    if (esp_timer_create(&args, &s_save_timer) != ESP_OK) {
        s_save_timer = NULL; // kept in RAM only
    }
    s_storage = storage_ok;
    if (storage_ok) {
        load();
    } else {
        nr_init(s_ring, 1);
    }
    nvs_handle_t nvs;
    uint8_t      dnd = 0;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, DND_KEY, &dnd);
        nvs_close(nvs);
    }
    atomic_store(&s_dnd, dnd != 0);
    atomic_store(&s_generation, s_ring->generation);
    atomic_store(&s_started, true);
    notify_cmd_register();
}

// ---- per-source switches --------------------------------------------------------------------------------------------

// The switch of source, from the cache or NVS (default on)
static bool source_on_locked(char const *source) {
    nmute_state_t state = nmute_get(&s_mute, source);
    if (state == NMUTE_UNKNOWN) {
        char         key[NMUTE_KEY_CAP];
        uint8_t      on = 1;
        nvs_handle_t nvs;
        if (nmute_key(source, key, sizeof(key)) && nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_u8(nvs, key, &on);
            nvs_close(nvs);
        }
        nmute_put(&s_mute, source, on != 0);
        state = on ? NMUTE_ON : NMUTE_OFF;
    }
    return state == NMUTE_ON;
}

bool lichen_notify_source_enabled(char const *source) {
    if (!atomic_load(&s_started) || !source || !source[0]) {
        return true;
    }
    lock();
    bool on = source_on_locked(source);
    unlock();
    return on;
}

void lichen_notify_set_source_enabled(char const *source, bool on) {
    char key[NMUTE_KEY_CAP];
    if (!atomic_load(&s_started) || !nmute_key(source, key, sizeof(key))) {
        return;
    }
    lock();
    nmute_put(&s_mute, source, on);
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = on ? nvs_erase_key(nvs, key) : nvs_set_u8(nvs, key, 0); // on is the default: no key
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "switch for %s not saved: %s", source, esp_err_to_name(err));
    }
}

// ---- raising --------------------------------------------------------------------------------------------------------

static uint32_t key_hash(char const *source, char const *target) {
    uint32_t h = 2166136261u; // FNV-1a over "source\0target"
    for (char const *p = source; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    h = (h ^ 0u) * 16777619u;
    for (char const *p = target ? target : ""; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

// LN_ONCE_BOOT: true when (source, target) was raised before this boot; remembers it otherwise
static bool seen_this_boot_locked(char const *source, char const *target) {
    uint32_t h = key_hash(source, target);
    for (size_t i = 0; i < s_once_n; i++) {
        if (s_once[i] == h) {
            return true;
        }
    }
    if (s_once_n < ONCE_SLOTS) {
        s_once[s_once_n++] = h;
    }
    return false;
}

uint32_t lichen_notify_to(char const *source, ln_level_t level, char const *target, char const *title,
                          char const *body, uint32_t flags) {
    if (!atomic_load(&s_started) || !source || !source[0]) {
        ESP_LOGW(TAG, "dropped \"%s\": %s", title ? title : "", source && source[0] ? "service not started" : "no source");
        return 0;
    }
    if (level > LN_ERROR) {
        level = LN_ERROR;
    }
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool     clock  = s_clock_set && s_clock_set();
    lock();
    source_on_locked(source);
    if (nmute_drop(&s_mute, source)) { // switched off in Settings › Notifications: no history, no badge
        unlock();
        return 0;
    }
    if ((flags & LN_ONCE_BOOT) && seen_this_boot_locked(source, target)) {
        unlock();
        return 0;
    }
    bool           within = nrate_take(&s_rate, source, now_ms);
    nr_new_t const n      = {
             .source     = source,
             .target     = target,
             .title      = title,
             .body       = body,
             .level      = (uint8_t)level,
             .flags      = (uint8_t)flags,
             .epoch      = clock ? (int64_t)time(NULL) : 0,
             .mono_s     = now_ms / 1000,
             .coalesce   = (flags & LN_COALESCE) || !within,
             .restricted = !within,
    };
    uint32_t     id      = 0;
    nr_outcome_t outcome = nr_add(s_ring, &n, &id);
    if (outcome != NR_DROPPED) {
        changed_locked(level == LN_ERROR);
    }
    nrate_show_t show = nrate_show((uint8_t)level, flags & LN_SILENT, atomic_load(&s_dnd), within);
    unlock();
    if (outcome == NR_DROPPED) {
        ESP_LOGW(TAG, "no room for \"%s\" from %s: dropped", title ? title : "", source);
        return 0;
    }
    ESP_LOGI(TAG, "%s %u from %s: %s%s", outcome == NR_COALESCED ? "merged into" : "new", (unsigned)id, source,
             title ? title : "", within ? "" : " (over its rate)");
    if (show.toast) {
        ln_toast_t const t = {.id = id, .level = (uint8_t)level, .feedback = show.feedback};
        xQueueSend(s_toasts, &t, 0); // a full queue loses the toast, never the entry
    }
    return id;
}

uint32_t lichen_notify(char const *source, ln_level_t level, char const *title, char const *body, uint32_t flags) {
    return lichen_notify_to(source, level, NULL, title, body, flags);
}

// ---- reading and changing -------------------------------------------------------------------------------------------

size_t lichen_notify_unread(char const *source) {
    if (!atomic_load(&s_started)) {
        return 0;
    }
    lock();
    size_t n = nr_unread(s_ring, source);
    unlock();
    return n;
}

ln_level_t lichen_notify_unread_top(void) {
    if (!atomic_load(&s_started)) {
        return LN_INFO;
    }
    lock();
    int top = nr_unread_top(s_ring);
    unlock();
    return top < 0 ? LN_INFO : (ln_level_t)top;
}

// Runs a change under the lock; mark-reads are batched with the slow save, removals of unread errors and clears soon
static void mark(char const *source, char const *target, uint32_t id, int upto) {
    if (!atomic_load(&s_started)) {
        return;
    }
    lock();
    size_t n = id ? nr_mark_read_id(s_ring, id)
               : upto >= 0 ? nr_mark_read_upto(s_ring, (uint8_t)upto)
                           : nr_mark_read(s_ring, source, target);
    if (n) {
        changed_locked(false);
    }
    unlock();
}

void lichen_notify_mark_read(char const *source, char const *target) {
    if (source) {
        mark(source, target, 0, -1);
    }
}

void lichen_notify_mark_read_id(uint32_t id) {
    if (id) {
        mark(NULL, NULL, id, -1);
    }
}

void lichen_notify_mark_read_upto(ln_level_t level) {
    mark(NULL, NULL, 0, (int)level);
}

void lichen_notify_mark_all_read(void) {
    mark(NULL, NULL, 0, -1);
}

bool lichen_notify_remove(uint32_t id) {
    if (!atomic_load(&s_started)) {
        return false;
    }
    lock();
    bool removed = nr_remove(s_ring, id);
    if (removed) {
        changed_locked(true);
    }
    unlock();
    return removed;
}

void lichen_notify_clear_all(void) {
    if (!atomic_load(&s_started)) {
        return;
    }
    lock();
    nr_clear(s_ring);
    changed_locked(true);
    unlock();
}

uint32_t lichen_notify_generation(void) {
    return atomic_load(&s_generation);
}

size_t lichen_notify_ids(uint32_t *ids, size_t max) {
    if (!atomic_load(&s_started)) {
        return 0;
    }
    lock();
    size_t n = nr_ids(s_ring, ids, max);
    unlock();
    return n;
}

bool lichen_notify_get(uint32_t id, nr_entry_t *out) {
    if (!atomic_load(&s_started)) {
        return false;
    }
    lock();
    nr_entry_t const *e = nr_get(s_ring, id);
    if (e) {
        *out = *e;
    }
    unlock();
    return e != NULL;
}

bool lichen_notify_next_toast(ln_toast_t *out) {
    return atomic_load(&s_started) && xQueueReceive(s_toasts, out, 0) == pdTRUE;
}

bool lichen_notify_dnd(void) {
    return atomic_load(&s_dnd);
}

void lichen_notify_set_dnd(bool on) {
    atomic_store(&s_dnd, on);
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, DND_KEY, on ? 1 : 0);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "do-not-disturb not saved: %s", esp_err_to_name(err));
    }
}

void lichen_notify_set_clock(bool (*is_set)(void)) {
    s_clock_set = is_set;
}

void lichen_notify_set_feedback(ln_feedback_t const *fb) {
    s_feedback = *fb;
}

void lichen_notify_feedback(ln_level_t level, bool on) {
    if (on && s_feedback.sound) {
        s_feedback.sound(level);
    }
    if (on && s_feedback.vibrate) {
        s_feedback.vibrate(level);
    }
    if (s_feedback.led) {
        s_feedback.led(level, on);
    }
}

void lichen_notify_stats(ln_stats_t *out) {
    memset(out, 0, sizeof(*out));
    out->started = atomic_load(&s_started);
    out->dnd     = atomic_load(&s_dnd);
    if (!out->started) {
        return;
    }
    lock();
    out->entries      = nr_count(s_ring);
    out->unread       = nr_unread(s_ring, NULL);
    out->dropped      = s_ring->dropped;
    out->muted        = s_mute.muted;
    out->boot         = s_ring->boot;
    out->storage      = s_storage;
    out->saves        = s_saves;
    out->save_errors  = s_save_errors;
    out->last_save_ms = s_last_save_ms;
    unlock();
}
