#include "lichen_fs.h"

#include "app_state.h"
#include "lichen_os.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fs_path.h"
#include "lichen_sys_app.h"
#include "sdcard.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

static char const TAG[] = "files";

// An app's directory on the storage drive and on the SD card are in its block (app_state.h, #520), so any number of apps
// each have theirs and they go with the app. Apps call lichen_fs_app_dir from their own tasks: the lookup and mkdir run
// with dirs_lock held (#272). The SD directory is made once per mount: a re-inserted (or another) card gets it again.
_Static_assert(sizeof(STORAGE_BASE) + 1 + 13 + 1 <= APP_STATE_DIR_CAP, "the storage directory path fits the app's block");
_Static_assert(sizeof(SD_APPS_DIR) + 1 + 13 + 1 <= APP_STATE_SD_CAP, "the SD directory path fits the app's block");

// Apps call lichen_fs_sd_dir from their own tasks: an app's SD directory (made, or rewritten after a remount) is only
// touched with this held. Both locks are mutexes, never a portMUX: mkdir runs under them, and the blocks are in
// PSRAM (no PSRAM in a critical section, #271). Separate, so a storage directory never waits on the SD card. Made on
// first use.
static SemaphoreHandle_t sd_dirs_lock, dirs_lock;
static StaticSemaphore_t sd_dirs_lock_buf, dirs_lock_buf;
static portMUX_TYPE      locks_make = portMUX_INITIALIZER_UNLOCKED;

static bool take_lock(SemaphoreHandle_t *lock, StaticSemaphore_t *buf) {
    portENTER_CRITICAL(&locks_make);
    if (!*lock) {
        *lock = xSemaphoreCreateMutexStatic(buf);
    }
    portEXIT_CRITICAL(&locks_make);
    return xSemaphoreTake(*lock, pdMS_TO_TICKS(2000)) == pdTRUE;
}

// The id of ctx's app for a directory call; NULL when the ctx is refused (the context table logged it), the OS's, or
// the id is unusable (or one of the OS's, #283). The id string lives as long as the app does: the tables key on it.
static char const *usable_id(lichen_ctx_t ctx, char const *what) {
    char const     *id   = NULL;
    char const     *name = NULL;
    gate_app_kind_t kind = lichen_sys_ctx_app(ctx, what, &id, &name);
    if (kind == GATE_APP_REFUSED) {
        return NULL;
    }
    char path[sizeof(STORAGE_BASE) + 1 + 13 + 1];
    if (kind != GATE_APP_ID || !fs_app_dir_path(STORAGE_BASE, id, path, sizeof(path))) { // the id rules, the OS's names
        ESP_LOGE(TAG, "%s has no usable id for its directory (or one of the OS's, #283)",
                 kind == GATE_APP_OS ? "the OS" : name ? name : "?");
        return NULL;
    }
    return id;
}

static bool made(char const *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

// With dirs_lock held
static char const *app_dir_locked(lichen_ctx_t ctx, char const *id) {
    app_state_t *st = lichen_sys_app_state(ctx);
    if (!st) {
        return NULL; // the app left
    }
    if (st->dir_made) {
        return st->dir_path;
    }
    if (!fs_app_dir_path(STORAGE_BASE, id, st->dir_path, sizeof(st->dir_path))) {
        return NULL; // usable_id already refused it; never mkdir a path that was not written
    }
    if (!made(st->dir_path)) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d): no drive?", st->dir_path, errno);
        return NULL; // not made: the next call tries again
    }
    st->dir_made = true;
    return st->dir_path;
}

char const *lichen_fs_app_dir(lichen_ctx_t ctx) {
    char const *id = usable_id(ctx, "lichen_fs_app_dir");
    if (!id) {
        return NULL;
    }
    // Once more on a timeout: a NULL here reads to the app like "no drive", so give a slow holder a second chance
    if (!take_lock(&dirs_lock, &dirs_lock_buf) && !take_lock(&dirs_lock, &dirs_lock_buf)) {
        ESP_LOGW(TAG, "%s: directory table busy (lock timeout twice), not a missing drive", id);
        return NULL;
    }
    char const *path = app_dir_locked(ctx, id);
    xSemaphoreGive(dirs_lock);
    return path;
}

bool lichen_fs_drive_bytes(size_t *total, size_t *used) {
    uint64_t free_bytes, total_bytes;
    if (!storage_free_bytes_cached(&free_bytes, &total_bytes)) { // MeshCore asks often: no walk of the drive (#300)
        *total = *used = 0;
        return false;
    }
    *total = (size_t)total_bytes;
    *used  = (size_t)(total_bytes - free_bytes);
    return true;
}

// With sd_dirs_lock held
static char const *sd_dir_locked(lichen_ctx_t ctx, char const *id, uint32_t generation) {
    app_state_t *st = lichen_sys_app_state(ctx);
    if (!st) {
        return NULL; // the app left
    }
    if (st->sd_path[0] && st->sd_generation == generation) {
        return st->sd_path;
    }
    fs_app_dir_path(SD_APPS_DIR, id, st->sd_path, sizeof(st->sd_path));
    if (!sd_begin(1000)) {
        return NULL;
    }
    bool ok  = made(SD_APPS_DIR) && made(st->sd_path);
    int  err = ok ? 0 : errno;
    sd_end(sd_io_from_errno(err)); // only a failing card counts toward the unmount, not ENOSPC or a name FAT refuses
    if (!ok) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d)", st->sd_path, err);
        return NULL;
    }
    st->sd_generation = generation;
    return st->sd_path;
}

char const *lichen_fs_sd_dir(lichen_ctx_t ctx) {
    if (!lichen_sys_caps_ok(ctx, CAP_SD, "lichen_fs_sd_dir")) {
        return NULL; // #229: an app that did not declare the card
    }
    char const *id = usable_id(ctx, "lichen_fs_sd_dir");
    if (!id) {
        return NULL;
    }
    sd_info_t info;
    sd_info(&info);
    if (info.state != SD_STATE_MOUNTED) {
        return NULL; // no card is normal: the app asks again later
    }
    if (!take_lock(&sd_dirs_lock, &sd_dirs_lock_buf)) {
        ESP_LOGW(TAG, "%s: SD directory table busy", id);
        return NULL;
    }
    char const *path = sd_dir_locked(ctx, id, info.generation);
    xSemaphoreGive(sd_dirs_lock);
    return path;
}

bool lichen_fs_sd_bytes(uint64_t *total, uint64_t *used) {
    sd_info_t info;
    sd_info(&info);
    if (info.state != SD_STATE_MOUNTED || !info.free_known) {
        *total = *used = 0;
        return false;
    }
    *total = info.total_bytes;
    *used  = info.total_bytes - info.free_bytes;
    return true;
}

static char const *volatile sd_holder;

// Who holds the batch lock: the ctx (0 = nobody) so only it may end the batch, and its app id for the log line of
// whoever waits in vain. Written under the lock, read racily: a hint, not a decision, except for the ctx compare in
// lichen_fs_sd_end, which only the holder can pass.
static volatile uint32_t sd_holder_ctx;
static TaskHandle_t volatile sd_holder_task; // who took the lock: a mutex is given back by its taker only

bool lichen_fs_sd_begin(lichen_ctx_t ctx, uint32_t wait_ms) {
    if (!lichen_sys_caps_ok(ctx, CAP_SD, "lichen_fs_sd_begin")) {
        return false; // #229: no batch, so the app calls no lichen_fs_sd_end either
    }
    char const     *id   = NULL;
    char const     *name = NULL;
    gate_app_kind_t kind = lichen_sys_ctx_caller(ctx, "lichen_fs_sd_begin", &id, &name);
    if (kind != GATE_APP_ID) {
        return false; // refused (logged), the OS's own, or an app without an id
    }
    if (!sd_begin(wait_ms)) {
        char const *holder = sd_holder;
        if (sd_mounted()) {
            ESP_LOGW(TAG, "%s: SD card busy (held by %s)", id, holder ? holder : "the OS");
        }
        return false;
    }
    sd_holder      = id;
    sd_holder_ctx  = ctx;
    sd_holder_task = xTaskGetCurrentTaskHandle();
    return true;
}

void lichen_fs_sd_end(lichen_ctx_t ctx, int err) {
    char const *id, *name;
    if (lichen_sys_ctx_caller(ctx, "lichen_fs_sd_end", &id, &name) == GATE_APP_REFUSED) {
        return; // a made-up ctx cannot end a batch somebody else holds
    }
    if (!gate_batch_may_end(sd_holder_ctx, ctx)) {
        ESP_LOGW(TAG, "%s: lichen_fs_sd_end without its own batch (held by %s): ignored", id ? id : "?",
                 sd_holder ? sd_holder : "nobody");
        return;
    }
    sd_holder      = NULL;
    sd_holder_ctx  = 0;
    sd_holder_task = NULL;
    sd_end(sd_io_from_errno(err));
}

bool lichen_fs_sd_mounted(void) {
    return sd_mounted();
}

// The loader's side (#533): an app that leaves must not keep the card locked. Ends the batch ctx still holds, when this very
// task took it (a FreeRTOS mutex given back by another task asserts); a batch another task took cannot be ended from here
// (that task is gone, or still running): logged.
void lichen_fs_sd_release_ctx(uint32_t ctx) {
    gate_rel_t act = gate_batch_release(sd_holder_ctx, (uintptr_t)sd_holder_task, ctx, (uintptr_t)xTaskGetCurrentTaskHandle());
    if (act == GATE_REL_NONE) {
        return;
    }
    if (act == GATE_REL_OTHER_TASK) {
        // The holder is another task of the same app: while it still runs (an engine task in a per-call read) it ends the batch
        // a moment later, so that is only news (#541); with no task of the app left nobody will, so it is a warning
        if (lichen_os_task_count(ctx) > 0) {
            ESP_LOGI(TAG, "%s: leaving while its own task holds its SD batch: it ends it when that call returns", sd_holder ? sd_holder : "?");
        } else {
            ESP_LOGW(TAG, "%s: leaving while another task holds its SD batch: the card stays locked until that task ends it or a restart", sd_holder ? sd_holder : "?");
        }
        return;
    }
    ESP_LOGW(TAG, "%s: left with the SD batch open: ended", sd_holder ? sd_holder : "?");
    sd_holder      = NULL;
    sd_holder_ctx  = 0;
    sd_holder_task = NULL;
    sd_end(SD_IO_NEUTRAL);
}
