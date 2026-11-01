#include "ota_valid.h"

bool ota_should_mark_valid(bool pending_verify, bool ui_up, bool c6_up, uint32_t uptime_s) {
    return pending_verify && ui_up && c6_up && uptime_s >= OTA_VALID_AFTER_S;
}

bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s) {
    return pending_verify && !c6_up && uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
}

bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id) {
    // magic 0xE9 at 0, chip id little-endian at 12 (esp_app_format.h)
    return head && len >= OTA_IMAGE_HEADER_LEN && head[0] == 0xE9 &&
           (uint16_t)(head[12] | (uint16_t)head[13] << 8) == chip_id;
}
