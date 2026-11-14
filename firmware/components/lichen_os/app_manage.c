// What Settings > Apps asks of the OS about one app (#233, lichen_sys_app_data.h): its data, its background switch and
// its crash count. Every call names an index of the OS's table: the app's id, its context and the paths all come from
// there, never from the caller, and every path is built by lichen_sys_data_fs.h from that id (one plain component below the
// drive's apps directory). No value of an app's settings is read, logged or counted.
#include "lichen_sys_app_data.h"

#include "lichen_sys_data_fs.h"
#include "esp_log.h"
#include "lichen_settings.h"
#include "lichen_sys_app.h"
#include "lichen_sys_storage.h"
#include "safe_mode.h"
#include "sdcard.h"

#include <errno.h>
#include <string.h>

static char const TAG[] = "app_data";

#define SD_WAIT_MS      3000
#define NVS_ENTRY_BYTES 32                               // one NVS entry

// The table's app at index and its context; false past the table or for an app with no usable id
static bool resolve(size_t index, lichen_app_t const **app, lichen_ctx_t *ctx) {
    *app = lichen_app_at(index);
    *ctx = lichen_sys_app_ctx_at(index);
    return *app && (*app)->id && *ctx != LICHEN_CTX_NONE;
}

// One directory on one drive under its lock; what is a measure or a clear of that directory
typedef bool (*dir_fn)(char const *dir, app_data_fs_t *out);

typedef enum {
    SD_NONE,   // no card mounted: nothing to do
    SD_BUSY,   // a card, but its lock was not free in time (or no path): not read, not cleared
    SD_DONE,   // the walk ran and worked
    SD_FAILED, // the walk ran and failed
} sd_walk_t;

// The app's directory on the SD card (SD_APPS_DIR, the one lichen_fs_sd_dir makes), under the card's batch lock
static sd_walk_t on_sd(lichen_app_t const *app, dir_fn fn, app_data_fs_t *fs) {
    char dir[APP_DATA_PATH_MAX];
    if (!lichen_sys_sd_mounted()) {
        return SD_NONE;
    }
    if (!lichen_sys_data_dir_path(SD_APPS_DIR, app->id, dir, sizeof(dir)) || !lichen_sys_sd_begin(SD_WAIT_MS)) {
        return SD_BUSY;
    }
    errno    = 0;
    bool ok  = fn(dir, fs);
    int  err = errno;
    lichen_sys_sd_end(ok ? LICHEN_SD_IO_OK : lichen_sys_sd_io_from_errno(err));
    return ok ? SD_DONE : SD_FAILED;
}

bool lichen_sys_app_data_measure(size_t index, lichen_app_data_t *out) {
    lichen_app_t const *app;
    lichen_ctx_t        ctx;
    memset(out, 0, sizeof(*out));
    if (!resolve(index, &app, &ctx)) {
        return false;
    }
    size_t        entries = 0;
    app_data_fs_t fs      = {0};
    char          dir[APP_DATA_PATH_MAX];
    bool          settings_ok = lichen_sys_app_settings_entries(ctx, &entries);
    bool          files_ok    = lichen_sys_data_dir_path(LICHEN_SYS_STORAGE_BASE, app->id, dir, sizeof(dir)) &&
                       lichen_sys_data_dir_measure(dir, &fs);
    out->settings_bytes = (uint32_t)(entries * NVS_ENTRY_BYTES);
    out->files_bytes    = fs.bytes;
    out->files          = fs.files;
    app_data_fs_t sd    = {0};
    sd_walk_t     card = on_sd(app, lichen_sys_data_dir_measure, &sd);
    out->sd_known = card == SD_DONE;
    out->sd_busy  = card == SD_BUSY;
    out->sd_bytes = sd.bytes;
    out->files += sd.files;
    out->ok = settings_ok && files_ok;
    return true;
}

bool lichen_sys_app_data_clear(size_t index, lichen_app_clear_t *out) {
    lichen_app_t const *app;
    lichen_ctx_t        ctx;
    memset(out, 0, sizeof(*out));
    // The OS's own apps and an app that keeps an identity and keys are not cleared here (the page does not offer it
    // either): a second check, since this is the call that erases
    if (!resolve(index, &app, &ctx) || (app->caps & CAP_SYSTEM) || app->storage_secret) {
        return false;
    }
    app_data_fs_t fs = {0}, sd = {0};
    char          dir[APP_DATA_PATH_MAX];
    out->settings_ok = app_settings_erase_all(ctx);
    out->files_ok    = lichen_sys_data_dir_path(LICHEN_SYS_STORAGE_BASE, app->id, dir, sizeof(dir)) && lichen_sys_data_dir_clear(dir, &fs);
    sd_walk_t card   = on_sd(app, lichen_sys_data_dir_clear, &sd);
    out->sd_ok       = card == SD_NONE || card == SD_DONE; // a busy card is not claimed clear
    out->bytes       = fs.bytes + sd.bytes;
    ESP_LOGI(TAG, "%s: data cleared, settings %s, files %s, card %s", app->id, out->settings_ok ? "ok" : "FAILED",
             out->files_ok ? "ok" : "FAILED", out->sd_ok ? "ok" : "FAILED");
    return out->settings_ok && out->files_ok && out->sd_ok;
}

// The switch is the person's for an app with a background part the OS does not keep on
static bool switchable(lichen_app_t const *app) {
    return app->start_background && !app->safe_mode_exempt && !(app->caps & CAP_SYSTEM);
}

bool lichen_sys_app_background_allowed(size_t index) {
    lichen_app_t const *app = lichen_app_at(index);
    return app && app->id && !safe_mode_background_listed(app->id);
}

bool lichen_sys_app_background_set(size_t index, bool allowed) {
    lichen_app_t const *app = lichen_app_at(index);
    return app && app->id && switchable(app) && safe_mode_background_set(app->id, allowed);
}

unsigned lichen_sys_app_crashes(size_t index, unsigned *window) {
    lichen_app_t const *app = lichen_app_at(index);
    *window                 = app && app->id ? SAFE_MODE_WINDOW_BOOTS : 0;
    return app && app->id ? safe_mode_app_crashes(app->id) : 0;
}
