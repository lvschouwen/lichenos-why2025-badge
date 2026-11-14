#include "lichen_settings.h"

#include "app_secret.h"
#include "lichen_sys_app_data.h"
#include "lichen_sys_app.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "settings_fail.h"

#include <string.h>

static char const TAG[] = "settings";

// Says once per ctx that an app (or the OS) asked for settings it cannot have: a few are remembered, so one busy app
// does not hide another's mistake; past those, silence
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
        } else if (i + 1 == sizeof(told) / sizeof(told[0])) {
            first = false; // table full and no match: stay quiet rather than log on every call
        }
    }
    portEXIT_CRITICAL(&mux);
    if (first) {
        ESP_LOGE(TAG, "%s has no settings namespace (the OS's, or no usable id)", name ? name : "the OS");
    }
}

// False for a ctx that is refused (logged by the context table), the OS's or an app without a usable id (said once
// here), or when NVS refuses; a namespace that was never written to does not exist yet, which is the quiet "no settings"
// answer for reads. *prefix gets the app's declared secret prefix (NULL: none).
static bool open_app(lichen_ctx_t ctx, nvs_open_mode_t mode, nvs_handle_t *out, char const **prefix) {
    char            ns[16];
    char const     *name = NULL;
    gate_app_kind_t kind = lichen_sys_ctx_settings_ns(ctx, "app_settings", ns, sizeof(ns), &name);
    if (kind != GATE_APP_ID) {
        if (kind != GATE_APP_REFUSED) { // a refused ctx is logged by the context table
            note_no_namespace(ctx, name);
        }
        return false;
    }
    lichen_app_t const *app = NULL;
    if (!lichen_sys_ctx_resolve(ctx, &app, "app_settings") || !app) {
        return false; // gone stale between the two looks: refused like any stale ctx
    }
    *prefix       = app->secret_prefix;
    esp_err_t err = nvs_open(ns, mode, out);
    if (err != ESP_OK && !(mode == NVS_READONLY && err == ESP_ERR_NVS_NOT_FOUND)) {
        ESP_LOGW(TAG, "%s: %s", ns, esp_err_to_name(err));
    }
    if (err != ESP_OK && mode == NVS_READWRITE) {
        settings_fail_note("(open)", err); // no key yet: the write that wanted this namespace never started
    }
    return err == ESP_OK;
}

// open_app for a plain key: a key of the app's secrets class (app_secret.h) is refused (the handle closed unused), for
// every type, so a secret is never read, written or erased through the plain calls (#288)
static bool open_plain(lichen_ctx_t ctx, char const *key, nvs_open_mode_t mode, nvs_handle_t *out) {
    char const *prefix = NULL;
    nvs_handle_t nvs;
    if (!key || !open_app(ctx, mode, &nvs, &prefix)) {
        return false;
    }
    if (app_secret_key_secret(prefix, key)) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "%s is in the app's secrets class: app_secret_* only", key); // the name, never a value
        return false;
    }
    *out = nvs;
    return true;
}

// open_app for a secret: stored gets its stored name; false for a key outside the app's declared prefix
static bool open_secret(lichen_ctx_t ctx, char const *key, nvs_open_mode_t mode, nvs_handle_t *out, char *stored,
                        size_t cap) {
    char const  *prefix = NULL;
    nvs_handle_t nvs;
    // #229: the secrets class is SECRETS (the descriptor's secret_prefix needs it, layers.conf rule D)
    if (!key || !lichen_sys_caps_ok(ctx, CAP_SECRETS, "app_secret") || !open_app(ctx, mode, &nvs, &prefix)) {
        return false;
    }
    if (!app_secret_stored_name(prefix, key, stored, cap)) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "%s is not under the app's secret prefix", key);
        return false;
    }
    *out = nvs;
    return true;
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
    if (!open_plain(ctx, key, NVS_READONLY, &nvs)) {
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
    if (!open_plain(ctx, key, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_u8(nvs, key, value));
}

uint32_t app_settings_get_u32(lichen_ctx_t ctx, char const *key, uint32_t def) {
    nvs_handle_t nvs;
    if (!open_plain(ctx, key, NVS_READONLY, &nvs)) {
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
    if (!open_plain(ctx, key, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_u32(nvs, key, value));
}

uint64_t app_settings_get_u64(lichen_ctx_t ctx, char const *key, uint64_t def) {
    nvs_handle_t nvs;
    if (!open_plain(ctx, key, NVS_READONLY, &nvs)) {
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
    if (!open_plain(ctx, key, NVS_READWRITE, &nvs)) {
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
    if (open_plain(ctx, key, NVS_READONLY, &nvs)) {
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
    if (!open_plain(ctx, key, NVS_READWRITE, &nvs)) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_str(nvs, key, value));
}

bool app_settings_erase(lichen_ctx_t ctx, char const *key) {
    nvs_handle_t nvs;
    if (!open_plain(ctx, key, NVS_READWRITE, &nvs)) {
        return false;
    }
    esp_err_t err = nvs_erase_key(nvs, key);
    return commit_close(key, nvs, err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}

// The whole namespace: the app's secrets and the move's record go with it (Clear data, #233)
bool app_settings_erase_all(lichen_ctx_t ctx) {
    size_t entries = 0;
    if (lichen_sys_app_settings_entries(ctx, &entries) && entries == 0) {
        return true; // nothing saved: opening it read-write would only create an empty namespace
    }
    char const  *prefix = NULL;
    nvs_handle_t nvs;
    if (!open_app(ctx, NVS_READWRITE, &nvs, &prefix)) {
        return false;
    }
    return commit_close("(all)", nvs, nvs_erase_all(nvs));
}

// Settings > Apps (#233): how many entries the app's namespace holds (a value of up to 8 bytes is one, a string or a
// blob takes more). Reads no value. A namespace that was never written is empty, not an error.
bool lichen_sys_app_settings_entries(lichen_ctx_t ctx, size_t *entries) {
    char            ns[16];
    char const     *name = NULL;
    nvs_handle_t    nvs;
    *entries = 0;
    if (lichen_sys_ctx_settings_ns(ctx, "app_settings", ns, sizeof(ns), &name) != GATE_APP_ID) {
        return false;
    }
    esp_err_t err = nvs_open(ns, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    err = nvs_get_used_entry_count(nvs, entries);
    nvs_close(nvs);
    return err == ESP_OK;
}

static void wipe(void *p, size_t n) {
    volatile char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

// No value is ever logged here; a failure is told by the key's name
bool app_secret_get(lichen_ctx_t ctx, char const *key, char *out, size_t cap) {
    if (!out || cap == 0) {
        return false;
    }
    out[0] = '\0';
    char         stored[APP_SECRET_STORED_MAX];
    nvs_handle_t nvs;
    if (!open_secret(ctx, key, NVS_READONLY, &nvs, stored, sizeof(stored))) {
        return false;
    }
    size_t    len = cap;
    esp_err_t err = nvs_get_str(nvs, stored, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        wipe(out, cap); // a short buffer may hold part of the value
        return false;
    }
    return true;
}

bool app_secret_set(lichen_ctx_t ctx, char const *key, char const *value) {
    char         stored[APP_SECRET_STORED_MAX];
    nvs_handle_t nvs;
    if (!value || strnlen(value, APP_SECRET_VALUE_MAX + 1) > APP_SECRET_VALUE_MAX ||
        !open_secret(ctx, key, NVS_READWRITE, &nvs, stored, sizeof(stored))) {
        return false;
    }
    return commit_close(key, nvs, nvs_set_str(nvs, stored, value));
}

bool app_secret_erase(lichen_ctx_t ctx, char const *key) {
    char         stored[APP_SECRET_STORED_MAX];
    nvs_handle_t nvs;
    if (!open_secret(ctx, key, NVS_READWRITE, &nvs, stored, sizeof(stored))) {
        return false;
    }
    esp_err_t err = nvs_erase_key(nvs, stored);
    return commit_close(key, nvs, err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}
