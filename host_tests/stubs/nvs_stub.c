#include "nvs.h"

#include <string.h>

#define SLOTS 8

static struct {
    char key[16];
    char value[130];
    int  used;
} table[SLOTS];

esp_err_t nvs_open(char const *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)name;
    (void)mode;
    *out = 1;
    return ESP_OK;
}

static int fail_in;

void nvs_stub_fail_set(int nth) {
    fail_in = nth;
}

esp_err_t nvs_set_str(nvs_handle_t h, char const *key, char const *value) {
    (void)h;
    if (fail_in > 0 && --fail_in == 0) {
        return ESP_FAIL;
    }
    if (strlen(key) >= sizeof(table[0].key) || strlen(value) >= sizeof(table[0].value)) {
        return ESP_FAIL;
    }
    int free_slot = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (table[i].used && strcmp(table[i].key, key) == 0) {
            strcpy(table[i].value, value);
            return ESP_OK;
        }
        if (!table[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return ESP_FAIL;
    }
    table[free_slot].used = 1;
    strcpy(table[free_slot].key, key);
    strcpy(table[free_slot].value, value);
    return ESP_OK;
}

// Like the real one: len is the buffer size in, the stored size including the NUL out
esp_err_t nvs_get_str(nvs_handle_t h, char const *key, char *out, size_t *len) {
    (void)h;
    for (int i = 0; i < SLOTS; i++) {
        if (table[i].used && strcmp(table[i].key, key) == 0) {
            size_t need = strlen(table[i].value) + 1;
            if (!out) {
                *len = need;
                return ESP_OK;
            }
            if (*len < need) {
                *len = need;
                return ESP_ERR_NVS_INVALID_LENGTH;
            }
            memcpy(out, table[i].value, need);
            *len = need;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_erase_key(nvs_handle_t h, char const *key) {
    (void)h;
    for (int i = 0; i < SLOTS; i++) {
        if (table[i].used && strcmp(table[i].key, key) == 0) {
            table[i].used = 0;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) {
    (void)h;
}
