// Host stand-in for ESP-IDF's nvs.h, for lichen_settings_test.c: the calls lichen_settings.c makes, kept per
// namespace by the test's own store (lichen_settings_test.c)
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int      esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_OK                     0
#define ESP_FAIL                   -1
#define ESP_ERR_NVS_NOT_FOUND      0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c

esp_err_t nvs_open(char const *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_set_str(nvs_handle_t h, char const *key, char const *value);
esp_err_t nvs_get_str(nvs_handle_t h, char const *key, char *out, size_t *len);
esp_err_t nvs_set_u8(nvs_handle_t h, char const *key, uint8_t value);
esp_err_t nvs_get_u8(nvs_handle_t h, char const *key, uint8_t *out);
esp_err_t nvs_set_u32(nvs_handle_t h, char const *key, uint32_t value);
esp_err_t nvs_get_u32(nvs_handle_t h, char const *key, uint32_t *out);
esp_err_t nvs_set_u64(nvs_handle_t h, char const *key, uint64_t value);
esp_err_t nvs_get_u64(nvs_handle_t h, char const *key, uint64_t *out);
esp_err_t nvs_erase_key(nvs_handle_t h, char const *key);
esp_err_t nvs_erase_all(nvs_handle_t h);
esp_err_t nvs_get_used_entry_count(nvs_handle_t h, size_t *used);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);
