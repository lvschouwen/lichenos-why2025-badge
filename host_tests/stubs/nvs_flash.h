// Host stand-in for ESP-IDF's nvs_flash.h: MeshCore's DataStore::formatFileSystem() erases NVS on ESP32 (#65)
#pragma once

typedef int esp_err_t;
#define ESP_OK 0

static inline esp_err_t nvs_flash_erase(void) {
    return ESP_OK;
}
