#include "lichen_fs.h"

#include "app_ns.h" // the id rules
#include "esp_log.h"
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

char const *lichen_fs_app_dir(lichen_app_t const *app) {
    char ns[16];
    if (!app || !app_ns_name(app->id, ns, sizeof(ns))) {
        ESP_LOGE(TAG, "app %s has no usable id for its directory", app && app->name ? app->name : "?");
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
    snprintf(slot->path, sizeof(slot->path), "%s/%s", STORAGE_BASE, app->id);
    if (mkdir(slot->path, 0755) != 0 && errno != EEXIST) {
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
