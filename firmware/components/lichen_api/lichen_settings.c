#include "lichen_settings.h"

#include "app_ns.h"
#include "esp_log.h"
#include "fw_console.h" // FW_NVS_NAMESPACE: where adopted keys used to live
#include "nvs.h"

#include <string.h>

static char const TAG[] = "settings";

// False (and a log line) for an app without a usable id, or when NVS refuses; a namespace that was never written to
// does not exist yet, which is the quiet "no settings" answer for reads
static bool open_app(lichen_app_t const *app, nvs_open_mode_t mode, nvs_handle_t *out) {
    char ns[16];
    if (!app || !app_ns_name(app->id, ns, sizeof(ns))) {
        ESP_LOGE(TAG, "app %s has no usable id for its settings", app && app->name ? app->name : "?");
        return false;
    }
    esp_err_t err = nvs_open(ns, mode, out);
    if (err != ESP_OK && !(mode == NVS_READONLY && err == ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG, "%s: %s", ns, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool commit_close(lichen_app_t const *app, char const *key, nvs_handle_t nvs, esp_err_t err) {
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: saving %s failed: %s", app->id, key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

uint8_t app_settings_get_u8(lichen_app_t const *app, char const *key, uint8_t def) {
    nvs_handle_t nvs;
    if (!open_app(app, NVS_READONLY, &nvs)) {
        return def;
    }
    uint8_t value = def;
    if (nvs_get_u8(nvs, key, &value) != ESP_OK) {
        value = def;
    }
    nvs_close(nvs);
    return value;
}

bool app_settings_set_u8(lichen_app_t const *app, char const *key, uint8_t value) {
    nvs_handle_t nvs;
    if (!open_app(app, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(app, key, nvs, nvs_set_u8(nvs, key, value));
}

void app_settings_get_str(lichen_app_t const *app, char const *key, char *out, size_t cap, char const *def) {
    if (cap == 0) {
        return;
    }
    nvs_handle_t nvs;
    size_t       len = cap;
    if (open_app(app, NVS_READONLY, &nvs)) {
        esp_err_t err = nvs_get_str(nvs, key, out, &len);
        nvs_close(nvs);
        if (err == ESP_OK) {
            out[cap - 1] = '\0';
            return;
        }
    }
    strncpy(out, def ? def : "", cap);
    out[cap - 1] = '\0';
}

bool app_settings_set_str(lichen_app_t const *app, char const *key, char const *value) {
    nvs_handle_t nvs;
    if (!open_app(app, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(app, key, nvs, nvs_set_str(nvs, key, value));
}

bool app_settings_erase(lichen_app_t const *app, char const *key) {
    nvs_handle_t nvs;
    if (!open_app(app, NVS_READWRITE, &nvs)) {
        return false;
    }
    esp_err_t err = nvs_erase_key(nvs, key);
    return commit_close(app, key, nvs, err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}

bool app_settings_erase_all(lichen_app_t const *app) {
    nvs_handle_t nvs;
    if (!open_app(app, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(app, "(all)", nvs, nvs_erase_all(nvs));
}

bool app_settings_adopt_u8(lichen_app_t const *app, char const *key) {
    nvs_handle_t nvs;
    uint8_t      value       = 0;
    bool         have_app    = false;
    bool         have_legacy = false;
    if (open_app(app, NVS_READONLY, &nvs)) {
        have_app = nvs_get_u8(nvs, key, &value) == ESP_OK;
        nvs_close(nvs);
    }
    if (!have_app && nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        have_legacy = nvs_get_u8(nvs, key, &value) == ESP_OK;
        nvs_close(nvs);
    }
    if (app_adopt_plan(have_app, have_legacy) != APP_ADOPT_COPY) {
        return false;
    }
    bool ok = app_settings_set_u8(app, key, value);
    ESP_LOGW(TAG, "%s: %s=%u adopted from %s (%s)", app->id, key, value, FW_NVS_NAMESPACE, ok ? "saved" : "failed");
    return ok;
}
