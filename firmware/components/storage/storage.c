#include "storage.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static char const TAG[] = "storage";

static bool mounted;

bool storage_mount(void) {
    storage_console_register(); // also on a failed mount: "fw storage" then says so
    static wl_handle_t                wl = WL_INVALID_HANDLE;
    esp_vfs_fat_mount_config_t const config = {
        // Never formatted on its own (#110): a drive that does not mount may still hold the only copy of the MeshCore
        // identity, and a fresh drive would let MeshCore make a new one. It stays unmounted; formatting is the user's
        // explicit choice.
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = STORAGE_CLUSTER,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_BASE, "storage", &config, &wl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting %s failed: %s; left unmounted and NOT formatted", STORAGE_BASE, esp_err_to_name(err));
        return false;
    }
    mounted = true;
    return true;
}

bool storage_mounted(void) {
    return mounted;
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
