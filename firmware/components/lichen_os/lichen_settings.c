#include "lichen_settings.h"

#include "lichen_sys_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "settings_fail.h"

#include <string.h>

static char const TAG[] = "settings";

// Says once per ctx that an app (or the OS) asked for settings it cannot have: a few are remembered, so one busy app
// does not hide another's mistake and none of them fills the log
static void note_no_namespace(lichen_ctx_t ctx, char const *name) {
    static lichen_ctx_t told[4];
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool                first = true;
    portENTER_CRITICAL(&mux);
    for (size_t i = 0; i < sizeof(told) / sizeof(told[0]) && first; i++) {
        if (told[i] == ctx) {
            first = false;
        } else if (told[i] == 0) {
            told[i] = ctx;
            break;
        }
    }
    portEXIT_CRITICAL(&mux);
    if (first) {
        ESP_LOGE(TAG, "%s has no settings namespace (the OS's, or no usable id)", name ? name : "the OS");
    }
}

// False for a ctx that is refused (logged by the context table), the OS's or an app without a usable id (said once
// here), or when NVS refuses; a namespace that was never written to does not exist yet, which is the quiet "no settings"
// answer for reads
static bool open_app(lichen_ctx_t ctx, nvs_open_mode_t mode, nvs_handle_t *out) {
    char            ns[16];
    char const     *name = NULL;
    gate_app_kind_t kind = lichen_sys_ctx_settings_ns(ctx, "app_settings", ns, sizeof(ns), &name);
    if (kind != GATE_APP_ID) {
        if (kind != GATE_APP_REFUSED) { // a refused ctx is logged by the context table
            note_no_namespace(ctx, name);
        }
        return false;
    }
    esp_err_t err = nvs_open(ns, mode, out);
    if (err != ESP_OK && !(mode == NVS_READONLY && err == ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG, "%s: %s", ns, esp_err_to_name(err));
    }
    if (err != ESP_OK && mode == NVS_READWRITE) {
        settings_fail_note("(open)", err); // no key yet: the write that wanted this namespace never started
    }
    return err == ESP_OK;
}

static bool commit_close(char const *key, nvs_handle_t nvs, esp_err_t err) {
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        settings_fail_note(key, err);
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

uint8_t app_settings_get_u8(lichen_ctx_t ctx, char const *key, uint8_t def) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READONLY, &nvs)) {
        return def;
    }
    uint8_t value = def;
    if (nvs_get_u8(nvs, key, &value) != ESP_OK) {
        value = def;
    }
    nvs_close(nvs);
    return value;
}

bool app_settings_set_u8(lichen_ctx_t ctx, char const *key, uint8_t value) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_u8(nvs, key, value));
}

uint32_t app_settings_get_u32(lichen_ctx_t ctx, char const *key, uint32_t def) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READONLY, &nvs)) {
        return def;
    }
    uint32_t value = def;
    if (nvs_get_u32(nvs, key, &value) != ESP_OK) {
        value = def;
    }
    nvs_close(nvs);
    return value;
}

bool app_settings_set_u32(lichen_ctx_t ctx, char const *key, uint32_t value) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_u32(nvs, key, value));
}

uint64_t app_settings_get_u64(lichen_ctx_t ctx, char const *key, uint64_t def) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READONLY, &nvs)) {
        return def;
    }
    uint64_t value = def;
    if (nvs_get_u64(nvs, key, &value) != ESP_OK) {
        value = def;
    }
    nvs_close(nvs);
    return value;
}

bool app_settings_set_u64(lichen_ctx_t ctx, char const *key, uint64_t value) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_u64(nvs, key, value));
}

void app_settings_get_str(lichen_ctx_t ctx, char const *key, char *out, size_t cap, char const *def) {
    if (cap == 0) {
        return;
    }
    nvs_handle_t nvs;
    size_t       len = cap;
    if (open_app(ctx, NVS_READONLY, &nvs)) {
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

bool app_settings_set_str(lichen_ctx_t ctx, char const *key, char const *value) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_str(nvs, key, value));
}

bool app_settings_erase(lichen_ctx_t ctx, char const *key) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    esp_err_t err = nvs_erase_key(nvs, key);
    return commit_close(key, nvs, err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}

bool app_settings_erase_all(lichen_ctx_t ctx) {
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close("(all)", nvs, nvs_erase_all(nvs));
}
