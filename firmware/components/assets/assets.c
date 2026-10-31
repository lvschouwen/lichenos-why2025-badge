#include "assets.h"

#include "assets_fmt.h"
#include "esp_log.h"
#include "esp_partition.h"

#include <stdio.h>

static char const TAG[] = "assets";

static assets_view_t view;
static bool          mounted;
static char          status[48] = "not mounted";

bool assets_mount(void) {
    esp_partition_t const *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASSETS_PARTITION);
    if (!part) {
        snprintf(status, sizeof(status), "no assets partition");
        ESP_LOGE(TAG, "%s", status);
        return false;
    }
    void const             *base;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &base, &handle);
    if (err != ESP_OK) {
        snprintf(status, sizeof(status), "mmap failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", status);
        return false;
    }
    assets_err_t res = assets_open(base, part->size, true, &view);
    if (res != ASSETS_OK) {
        esp_partition_munmap(handle);
        snprintf(status, sizeof(status), "%s", assets_err_str(res));
        ESP_LOGE(TAG, "%s: using built-in fonts", status);
        return false;
    }
    mounted = true; // stays mapped until reboot: callers hold pointers into it
    snprintf(status, sizeof(status), "%u files, %u KB", (unsigned)view.count, (unsigned)(view.total_len / 1024));
    ESP_LOGW(TAG, "%s", status);
    return true;
}

bool assets_get(char const *name, uint8_t const **data, uint32_t *size) {
    return mounted && assets_find(&view, name, data, size);
}

char const *assets_status(void) {
    return status;
}
