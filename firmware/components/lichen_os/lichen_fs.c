#include "lichen_fs.h"

#include "app_slot.h"
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

// An app's directory on the storage drive: its owner and its path side by side, so app_slot_pick (pure) reads the
// owners. Apps call lichen_fs_app_dir from their own tasks: the lookup, mkdir and claim run with dirs_lock held (#272).
EXT_RAM_BSS_ATTR static void const *dir_owner[LICHEN_MAX_APPS]; // the app's id string
EXT_RAM_BSS_ATTR static char        dir_path[LICHEN_MAX_APPS][sizeof(STORAGE_BASE) + 1 + 13 + 1]; // + the longest id

// An app's directory on the SD card, made once per mount: a re-inserted (or another) card gets it again
typedef struct {
    char const *id;
    uint32_t    generation; // sd_info's generation it was made in; 0 = not yet
    char        path[sizeof(SD_APPS_DIR) + 1 + 13 + 1];
} sd_dir_t;

EXT_RAM_BSS_ATTR static sd_dir_t sd_dirs[LICHEN_MAX_APPS];

// Apps call lichen_fs_sd_dir from their own tasks: the table (claiming a slot, rewriting it after a remount) is only
// touched with this held. Both tables' locks are mutexes, never a portMUX: mkdir runs under them, and the tables are in
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
static char const *app_dir_locked(char const *id) {
    size_t i = app_slot_pick(dir_owner, LICHEN_MAX_APPS, id);
    if (i == LICHEN_MAX_APPS) {
        ESP_LOGE(TAG, "no room for %s's directory", id);
        return NULL;
    }
    if (dir_owner[i] == id) {
        return dir_path[i];
    }
    if (!fs_app_dir_path(STORAGE_BASE, id, dir_path[i], sizeof(dir_path[i]))) {
        return NULL; // usable_id already refused it; never mkdir a path that was not written
    }
    if (!made(dir_path[i])) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d): no drive?", dir_path[i], errno);
        return NULL; // the slot stays free: the next call tries again
    }
    dir_owner[i] = id;
    return dir_path[i];
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
    char const *path = app_dir_locked(id);
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
static char const *sd_dir_locked(char const *id, uint32_t generation) {
    sd_dir_t *slot = NULL;
    for (size_t i = 0; i < LICHEN_MAX_APPS; i++) {
        if (sd_dirs[i].id == id) {
            slot = &sd_dirs[i];
            break;
        }
        if (!slot && !sd_dirs[i].id) {
            slot = &sd_dirs[i];
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no room for %s's SD directory", id);
        return NULL;
    }
    if (slot->id == id && slot->generation == generation) {
        return slot->path;
    }
    fs_app_dir_path(SD_APPS_DIR, id, slot->path, sizeof(slot->path));
    slot->id = id;
    if (!sd_begin(1000)) {
        return NULL;
    }
    bool ok  = made(SD_APPS_DIR) && made(slot->path);
    int  err = ok ? 0 : errno;
    sd_end(sd_io_from_errno(err)); // only a failing card counts toward the unmount, not ENOSPC or a name FAT refuses
    if (!ok) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d)", slot->path, err);
        return NULL;
    }
    slot->generation = generation;
    return slot->path;
}

char const *lichen_fs_sd_dir(lichen_ctx_t ctx) {
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
    char const *path = sd_dir_locked(id, info.generation);
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

bool lichen_fs_sd_begin(lichen_ctx_t ctx, uint32_t wait_ms) {
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
    sd_holder     = id;
    sd_holder_ctx = ctx;
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
    sd_holder     = NULL;
    sd_holder_ctx = 0;
    sd_end(sd_io_from_errno(err));
}

bool lichen_fs_sd_mounted(void) {
    return sd_mounted();
}
