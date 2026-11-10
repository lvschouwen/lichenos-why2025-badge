#include "ota_valid.h"

static bool os_healthy(ota_trial_in_t const *in) {
    return in->boot_done && in->c6_up && !in->psram_trial && (!in->display || in->shell_up);
}

ota_trial_act_t ota_trial_step(ota_trial_in_t const *in) {
    if (!in->pending) {
        return OTA_TRIAL_DONE;
    }
    if (in->uptime_s >= OTA_VALID_AFTER_S && os_healthy(in)) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (ota_should_roll_back_now(in->pending, in->c6_up, in->uptime_s)) {
        return OTA_TRIAL_ROLL_BACK;
    }
    if (in->uptime_s >= OTA_VALID_AFTER_S && !in->psram_trial) {
        return OTA_TRIAL_STAY;
    }
    return OTA_TRIAL_WAIT;
}

bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s) {
    return pending_verify && !c6_up && uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
}

bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id) {
    // magic 0xE9 at 0, chip id little-endian at 12 (esp_app_format.h)
    return head && len >= OTA_IMAGE_HEADER_LEN && head[0] == 0xE9 &&
           (uint16_t)(head[12] | (uint16_t)head[13] << 8) == chip_id;
}
