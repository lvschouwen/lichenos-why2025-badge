#include "storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"

static char const TAG[] = "storage";

static bool mounted;

bool storage_attach(void) {
    esp_vfs_littlefs_conf_t const conf = {
        .base_path       = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        // Never formatted on its own (#110): a drive that does not mount may still hold the only copy of the MeshCore
        // identity, and a fresh drive would let MeshCore make a new one. It stays unmounted; formatting is the user's
        // explicit choice. A badge still on the FAT drive of before #300 lands here too: the USB route moves it.
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mounting %s failed: %s; left unmounted and NOT formatted", STORAGE_BASE, esp_err_to_name(err));
        return false;
    }
    return true;
}

void storage_detach(void) {
    esp_vfs_littlefs_unregister(STORAGE_LABEL);
}

bool storage_format(void) {
    esp_err_t err = esp_littlefs_format(STORAGE_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "formatting %s failed: %s", STORAGE_BASE, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool storage_mount(void) {
    storage_console_register(); // also on a failed mount: "fw storage" then says so
    mounted = storage_attach();
    return mounted;
}

bool storage_mounted(void) {
    return mounted;
}

bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes) {
    size_t total, used;
    if (esp_littlefs_info(STORAGE_LABEL, &total, &used) != ESP_OK) {
        return false;
    }
    *free_bytes  = used < total ? total - used : 0;
    *total_bytes = total;
    return true;
}
