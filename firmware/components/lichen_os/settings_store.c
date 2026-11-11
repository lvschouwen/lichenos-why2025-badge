#include "settings_store.h"

#include "backlight.h"
#include "esp_log.h"
#include "fw_console.h"
#include "haptics.h"
#include "led.h"
#include "lichen_os.h"
#include "notify_bind.h"
#include "nvs.h"
#include "sensors.h"
#include "settings_fail.h"
#include "settings_secret.h"
#include "wifi_list.h"

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
        settings_fail_note(key, err);
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

void settings_get_str(char const *key, char *out, size_t cap, char const *def) {
    if (cap == 0) {
        return;
    }
    nvs_handle_t nvs;
    size_t       len = cap;
    // A secret reads as missing here: only settings_secret_get() hands one out (#235)
    if (!settings_key_secret(FW_NVS_NAMESPACE, key) && nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, key, out, &len);
        nvs_close(nvs);
        if (err == ESP_OK) {
            return;
        }
    }
    strlcpy(out, def, cap);
}

bool settings_set_str(char const *key, char const *value) {
    if (settings_key_secret(FW_NVS_NAMESPACE, key)) {
        ESP_LOGE(TAG, "%s is a secret: settings_secret_set", key);
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
        settings_fail_note(key, err);
        ESP_LOGE(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool settings_get_blob(char const *key, void *out, size_t size) {
    nvs_handle_t nvs;
    if (settings_key_secret(FW_NVS_NAMESPACE, key)) { // a secret is never handed out through the plain API
        return false;
    }
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t    len = size;
    esp_err_t err = nvs_get_blob(nvs, key, out, &len);
    nvs_close(nvs);
    return err == ESP_OK && len == size;
}

// No log line: the restart history writes this on the way into esp_restart()
bool settings_set_blob(char const *key, void const *data, size_t size) {
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, key, data, size);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        settings_fail_note(key, err);
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
    if (err != ESP_OK) {
        settings_fail_note(key, err);
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

void settings_bind_services(void) {
    backlight_bind_store(settings_get_u8, settings_set_u8);
    sensors_bind_store(settings_get_u8, settings_set_u8, settings_erase);
    lichen_notify_bind_store(settings_get_u8, settings_set_u8);
    led_bind_store(settings_get_u8, settings_set_u8);
    haptics_bind_store(settings_get_u8, settings_set_u8);
    system_restart_bind_store(settings_get_blob, settings_set_blob);
    os_bind_lower();
}

// Separate from settings_bind_services() on purpose (#235): the Wi-Fi credentials are the secrets class's first user
static wifi_store_t const net_store = {
    .get_str    = settings_get_str,
    .set_str    = settings_set_str,
    .erase      = settings_erase,
    .get_u8     = settings_get_u8,
    .set_u8     = settings_set_u8,
    .secret_get = settings_secret_get,
    .secret_set = settings_secret_set,
};

void settings_bind_net(void) {
    wifi_list_bind_store(&net_store);
}
