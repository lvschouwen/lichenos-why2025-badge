#include "lichen_fs.h"

#include "app_ns.h" // the id rules
#include "esp_log.h"
#include "fs_path.h"
#include "sdcard.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

static char const TAG[] = "files";

typedef struct {
    lichen_app_t const *app;
    char                path[sizeof(STORAGE_BASE) + 1 + 13 + 1]; // "/storage/" + the longest id
} app_dir_t;

static app_dir_t dirs[LICHEN_MAX_APPS];

// An app's directory on the SD card, made once per mount: a re-inserted (or another) card gets it again
typedef struct {
    lichen_app_t const *app;
    uint32_t            generation; // sd_info's generation it was made in; 0 = not yet
    char                path[sizeof(SD_APPS_DIR) + 1 + 13 + 1];
} sd_dir_t;

static sd_dir_t sd_dirs[LICHEN_MAX_APPS];

static bool usable(lichen_app_t const *app) {
    char ns[16];
    if (!app || !app_ns_name(app->id, ns, sizeof(ns))) {
        ESP_LOGE(TAG, "app %s has no usable id for its directory", app && app->name ? app->name : "?");
        return false;
    }
    return true;
}

static bool made(char const *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

char const *lichen_fs_app_dir(lichen_app_t const *app) {
    if (!usable(app)) {
        return NULL;
    }
    app_dir_t *slot = NULL;
    for (size_t i = 0; i < LICHEN_MAX_APPS; i++) {
        if (dirs[i].app == app) {
            return dirs[i].path;
        }
        if (!slot && !dirs[i].app) {
            slot = &dirs[i];
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no room for %s's directory", app->id);
        return NULL;
    }
    fs_app_dir_path(STORAGE_BASE, app->id, slot->path, sizeof(slot->path));
    if (!made(slot->path)) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d): no drive?", slot->path, errno);
        return NULL;
    }
    slot->app = app;
    return slot->path;
}

bool lichen_fs_drive_bytes(size_t *total, size_t *used) {
    uint64_t free_bytes, total_bytes;
    if (!storage_free_bytes(&free_bytes, &total_bytes)) {
        *total = *used = 0;
        return false;
    }
    *total = (size_t)total_bytes;
    *used  = (size_t)(total_bytes - free_bytes);
    return true;
}

char const *lichen_fs_sd_dir(lichen_app_t const *app) {
    if (!usable(app)) {
        return NULL;
    }
    sd_info_t info;
    sd_info(&info);
    if (info.state != SD_STATE_MOUNTED) {
        return NULL; // no card is normal: the app asks again later
    }
    sd_dir_t *slot = NULL;
    for (size_t i = 0; i < LICHEN_MAX_APPS; i++) {
        if (sd_dirs[i].app == app) {
            slot = &sd_dirs[i];
            break;
        }
        if (!slot && !sd_dirs[i].app) {
            slot = &sd_dirs[i];
        }
    }
    if (!slot) {
        ESP_LOGE(TAG, "no room for %s's SD directory", app->id);
        return NULL;
    }
    if (slot->app == app && slot->generation == info.generation) {
        return slot->path;
    }
    fs_app_dir_path(SD_APPS_DIR, app->id, slot->path, sizeof(slot->path));
    slot->app = app;
    if (!sd_begin(1000)) {
        return NULL;
    }
    bool ok = made(SD_APPS_DIR) && made(slot->path);
    int  err = errno;
    sd_end(ok);
    if (!ok) {
        ESP_LOGE(TAG, "%s: mkdir failed (errno %d)", slot->path, err);
        return NULL;
    }
    slot->generation = info.generation;
    return slot->path;
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
