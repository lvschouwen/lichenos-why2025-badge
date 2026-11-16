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

// The entry walk (nvs_flash.h's stand-in includes this): the test's store answers it, in storage order
#define NVS_DEFAULT_PART_NAME "nvs"
typedef enum { NVS_TYPE_STR, NVS_TYPE_ANY } nvs_type_t;
typedef struct nvs_opaque_iterator *nvs_iterator_t;
typedef struct {
    char namespace_name[16];
    char key[16];
} nvs_entry_info_t;
esp_err_t nvs_entry_find(char const *part, char const *ns, nvs_type_t type, nvs_iterator_t *out);
esp_err_t nvs_entry_next(nvs_iterator_t *it);
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info);
void      nvs_release_iterator(nvs_iterator_t it);
