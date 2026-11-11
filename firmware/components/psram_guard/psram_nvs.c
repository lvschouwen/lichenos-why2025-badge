#include "psram_nvs.h"

#include "fw_console.h"
#include "nvs_fail.h"

static char const TAG[] = "psram";

esp_err_t psram_nvs_open(char const *key, nvs_handle_t *out) {
    esp_err_t err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, out);
    fw_nvs_ok(TAG, key, err);
    return err;
}

esp_err_t psram_nvs_put(nvs_handle_t nvs, char const *key, void const *blob, size_t len) {
    esp_err_t err = nvs_set_blob(nvs, key, blob, len);
    if (fw_nvs_ok(TAG, key, err)) {
        err = nvs_commit(nvs);
        fw_nvs_ok(TAG, key, err);
    }
    return err;
}

esp_err_t psram_nvs_drop(nvs_handle_t nvs, char const *key) {
    esp_err_t err = nvs_erase_key(nvs, key);
    if (!fw_nvs_erased(TAG, key, err)) {
        return err;
    }
    err = nvs_commit(nvs);
    fw_nvs_ok(TAG, key, err);
    return err;
}
