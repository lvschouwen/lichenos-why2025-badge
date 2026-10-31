#include "storage.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static char const TAG[] = "storage";

bool storage_mount(void) {
    static wl_handle_t                wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t const config = {
        .format_if_mount_failed = true, // a blank or damaged drive is better than no drive
        .max_files              = 8,
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_BASE, "storage", &config, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting %s failed: %s", STORAGE_BASE, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes) {
    uint64_t total, avail;
    if (esp_vfs_fat_info(STORAGE_BASE, &total, &avail) != ESP_OK) {
        return false;
    }
    *free_bytes  = avail;
    *total_bytes = total;
    return true;
}
