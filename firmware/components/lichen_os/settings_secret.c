#include "settings_secret.h"

#include "fw_console.h"
#include "nvs.h"
#include "settings_fail.h"

#include <string.h>

// No logging here: a failure is the caller's to report, and a log line is one slip away from printing a value

bool settings_secret_get(char const *key, char *out, size_t cap) {
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (!settings_key_secret(FW_NVS_NAMESPACE, key)) {
        return false;
    }
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t    len = cap;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    nvs_close(nvs);
    if (err != ESP_OK) {
        memset(out, 0, cap); // a short buffer may hold part of the value
        return false;
    }
    return true;
}

bool settings_secret_set(char const *key, char const *value) {
    if (!value || !settings_key_secret(FW_NVS_NAMESPACE, key)) {
        return false;
    }
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        settings_fail_note(key, err); // the key's name only, never the value
    }
    return err == ESP_OK;
}
