// Host stand-in for ESP-IDF's nvs.h: an in-memory key/value store for strings (nvs_stub.c), enough for the Wi-Fi
// credentials test. All namespaces share one table.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef int      esp_err_t;
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

#define ESP_OK                    0
#define ESP_FAIL                  -1
#define ESP_ERR_NVS_NOT_FOUND     0x1102
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c

esp_err_t nvs_open(char const *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_set_str(nvs_handle_t h, char const *key, char const *value);
esp_err_t nvs_get_str(nvs_handle_t h, char const *key, char *out, size_t *len);
esp_err_t nvs_erase_key(nvs_handle_t h, char const *key);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);

// Test hook: the nth nvs_set_str call from now on fails (1 = the next one); 0 switches it off
void nvs_stub_fail_set(int nth);
