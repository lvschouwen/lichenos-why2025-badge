#include "assets.h"

#include "assets_fmt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "assets";

static assets_view_t view;
static uint8_t      *copy; // the image in PSRAM while mounted
static char          status[48] = "not mounted";

// Read, not mmap: while any flash stays mapped, a flash write turns the cache and non-IRAM interrupts off and the
// display drops frames (#31); unmapping at runtime hung the badge.
bool assets_mount(void) {
    esp_partition_t const *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASSETS_PARTITION);
    if (!part) {
        snprintf(status, sizeof(status), "no assets partition");
        ESP_LOGE(TAG, "%s", status);
        return false;
    }
    uint8_t   header[ASSETS_HEADER_LEN];
    esp_err_t err = esp_partition_read(part, 0, header, sizeof(header));
    uint32_t  len = ASSETS_HEADER_LEN;
    if (err == ESP_OK) {
        uint32_t total;
        memcpy(&total, header + 12, sizeof(total)); // total_len, little endian like the P4
        // A blank or foreign partition: let assets_open() name the error from the header alone
        len = total > ASSETS_HEADER_LEN && total <= part->size ? total : ASSETS_HEADER_LEN;
        copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        err  = copy ? esp_partition_read(part, 0, copy, len) : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        assets_unmount();
        snprintf(status, sizeof(status), "read failed (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", status);
        return false;
    }
    assets_err_t res = assets_open(copy, len, true, &view);
    if (res != ASSETS_OK) {
        assets_unmount();
        snprintf(status, sizeof(status), "%s", assets_err_str(res));
        ESP_LOGE(TAG, "%s: using built-in fonts", status);
        return false;
    }
    snprintf(status, sizeof(status), "%u files, %u KB", (unsigned)view.count, (unsigned)(view.total_len / 1024));
    ESP_LOGW(TAG, "%s", status);
    return true;
}

void assets_unmount(void) {
    heap_caps_free(copy);
    copy = NULL;
}

bool assets_get(char const *name, uint8_t const **data, uint32_t *size) {
    return copy && assets_find(&view, name, data, size);
}

char const *assets_status(void) {
    return status;
}
