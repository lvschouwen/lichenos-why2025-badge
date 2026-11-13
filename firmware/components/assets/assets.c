#include "assets.h"

#include "assets_fmt.h"
#include "esp_heap_caps.h"
#include "flash_chunk.h"
#include "esp_log.h"
#include "esp_partition.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "assets";

static assets_view_t view;
static uint8_t      *copy; // the image in PSRAM while mounted
static char          status[48] = "not mounted";

// A partition read for flash_chunk_copy that keeps the error, so the status names it
typedef struct {
    esp_partition_t const *part;
    esp_err_t              err;
} part_read_t;

static int part_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    part_read_t *r = ctx;
    r->err         = esp_partition_read(r->part, off, buf, n);
    return r->err == ESP_OK ? 0 : -1;
}

// The image into copy through a transient 4 KB internal bounce, freed right after; the stack one when that malloc fails
// (#366). Keeps the partition's own error so the status names it.
static esp_err_t copy_image(esp_partition_t const *part, uint32_t len) {
    uint8_t     bounce[FLASH_CHUNK_BOUNCE];
    part_read_t r   = {.part = part, .err = ESP_OK};
    uint8_t    *big = heap_caps_malloc(FLASH_CHUNK_BIG, MALLOC_CAP_INTERNAL);
    if (!big) {
        ESP_LOGW(TAG, "no internal bounce: reading through the stack, slower");
    }
    size_t   cap = 0;
    uint8_t *via = flash_chunk_pick(big, FLASH_CHUNK_BIG, bounce, sizeof(bounce), &cap);
    int      rc  = flash_chunk_copy(part_read, &r, 0, copy, len, via, cap);
    heap_caps_free(big);
    return rc == 0 ? ESP_OK : r.err != ESP_OK ? r.err : ESP_FAIL;
}

// Read, not mmap: while any flash stays mapped, a flash write turns the cache and non-IRAM interrupts off and the
// display drops frames (#31); unmapping at runtime hung the badge. The image goes to PSRAM through an internal bounce
// (copy_image): one read straight into PSRAM took a 16 KB internal temp buffer at boot (#364), a 512 B bounce alone made
// the boot mount ~100 ms slower (#366).
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
        err  = copy ? copy_image(part, len) : ESP_ERR_NO_MEM;
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
    ESP_LOGI(TAG, "%s", status);
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
