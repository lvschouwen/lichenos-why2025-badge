#include "lichen_legacy.h"

#include "esp_log.h"
#include "fw_console.h" // FW_NVS_NAMESPACE
#include "nvs.h"

static char const TAG[] = "legacy";

typedef struct {
    char const *key;
} blob_ctx_t;

static lb_read_t blob_read(void *ctx, uint8_t *buf, size_t cap, size_t *len) {
    blob_ctx_t const *c = ctx;
    nvs_handle_t      nvs;
    esp_err_t         err = nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        *len = cap;
        err  = nvs_get_blob(nvs, c->key, buf, len);
        nvs_close(nvs);
    }
    if (err == ESP_OK) {
        return LB_READ_FOUND;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return LB_READ_MISSING;
    }
    ESP_LOGW(TAG, "%s/%s unreadable: %s", FW_NVS_NAMESPACE, c->key, esp_err_to_name(err));
    return LB_READ_ERROR;
}

static bool blob_erase(void *ctx) {
    blob_ctx_t const *c = ctx;
    nvs_handle_t      nvs;
    esp_err_t         err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, c->key);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "old %s/%s not erased: %s", FW_NVS_NAMESPACE, c->key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

lichen_blob_t lichen_legacy_blob_take(lichen_app_t const *app, char const *key, uint8_t *buf, size_t cap, size_t min_len,
                                      lichen_blob_store_fn store, void *store_ctx) {
    if (!app || !legacy_blob_allowed(app->id, key)) {
        ESP_LOGE(TAG, "%s may not take %s", app && app->id ? app->id : "?", key ? key : "?");
        return LICHEN_BLOB_NONE;
    }
    blob_ctx_t     ctx = {.key = key};
    lb_io_t const io  = {.ctx = &ctx, .read = blob_read, .erase = blob_erase};
    return legacy_blob_run(&io, buf, cap, min_len, store, store_ctx);
}
