#include "storage.h"

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"

static char const TAG[] = "storage";

static bool mounted;

// The last measured free space (storage_free_bytes), for readers that must not walk the drive (#300)
static portMUX_TYPE s_free_mux = portMUX_INITIALIZER_UNLOCKED;
static bool         s_free_known;
static uint64_t     s_free, s_total;

static bool partition_is_littlefs(void) {
    esp_partition_t const *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, STORAGE_LABEL);
    if (p && p->subtype == ESP_PARTITION_SUBTYPE_DATA_LITTLEFS) {
        return true;
    }
    ESP_LOGE(TAG, "no LittleFS partition \"%s\" in the partition table", STORAGE_LABEL);
    return false;
}

bool storage_attach(void) {
    if (!partition_is_littlefs()) {
        return false;
    }
    esp_vfs_littlefs_conf_t const conf = {
        .base_path       = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        // Never formatted on its own (#110): a drive that does not mount may still hold the only copy of the MeshCore
        // identity, and a fresh drive would let MeshCore make a new one. It stays unmounted; formatting is the user's
        // explicit choice.
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
    if (!partition_is_littlefs()) {
        return false;
    }
    esp_err_t err = esp_littlefs_format(STORAGE_LABEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "formatting %s failed: %s", STORAGE_BASE, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool storage_mount(void) {
    storage_console_register(); // also on a failed mount: "fw storage" then says so
    mounted = storage_attach();
    if (mounted) {
        uint64_t free_b, total_b;
        storage_free_bytes(&free_b, &total_b); // About and MeshCore read the cache before sys_notices first does
    }
    return mounted;
}

bool storage_mounted(void) {
    return mounted;
}

bool storage_free_bytes(uint64_t *free_bytes, uint64_t *total_bytes) {
    size_t total, used;
    // esp_littlefs folds an lfs_fs_size error into "used = total" (it clamps the negative count), so a drive with
    // nothing free reads as unknown, never as 0 B free: the callers then let a write try and report its own error
    bool ok = esp_littlefs_info(STORAGE_LABEL, &total, &used) == ESP_OK && used < total;
    taskENTER_CRITICAL(&s_free_mux);
    s_free_known = ok;
    s_free       = ok ? total - used : 0;
    s_total      = ok ? total : 0;
    taskEXIT_CRITICAL(&s_free_mux);
    if (ok) {
        *free_bytes  = total - used;
        *total_bytes = total;
    }
    return ok;
}

bool storage_free_bytes_cached(uint64_t *free_bytes, uint64_t *total_bytes) {
    taskENTER_CRITICAL(&s_free_mux);
    bool ok = s_free_known;
    if (ok) {
        *free_bytes  = s_free;
        *total_bytes = s_total;
    }
    taskEXIT_CRITICAL(&s_free_mux);
    return ok;
}
