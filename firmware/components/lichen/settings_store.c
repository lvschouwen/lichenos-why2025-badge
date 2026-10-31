#include "settings_store.h"

#include "esp_log.h"
#include "fw_console.h"
#include "nvs.h"

#include "tz_table.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static char const TAG[] = "settings";

uint8_t settings_get_u8(char const *key, uint8_t def) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return def;
    }
    uint8_t value = def;
    if (nvs_get_u8(nvs, key, &value) != ESP_OK) {
        value = def;
    }
    nvs_close(nvs);
    return value;
}

bool settings_set_u8(char const *key, uint8_t value) {
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void settings_get_str(char const *key, char *out, size_t cap, char const *def) {
    if (cap == 0) {
        return;
    }
    nvs_handle_t nvs;
    size_t       len = cap;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, key, out, &len);
        nvs_close(nvs);
        if (err == ESP_OK) {
            return;
        }
    }
    strlcpy(out, def, cap);
}

bool settings_set_str(char const *key, char const *value) {
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
        ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool settings_erase(char const *key) {
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_erase_key(nvs, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        } else if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return err == ESP_OK;
}

void settings_apply_tz(void) {
    char tz[64];
    settings_get_str("tz", tz, sizeof(tz), TZ_DEFAULT_POSIX);
    setenv("TZ", tz, 1);
    tzset();
}

bool settings_set_tz(char const *posix) {
    bool saved = settings_set_str("tz", posix);
    setenv("TZ", posix, 1);
    tzset();
    return saved;
}
