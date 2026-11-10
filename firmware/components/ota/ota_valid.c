#include "ota_valid.h"

static bool proven(ota_trial_in_t const *in) {
    bool ui_ok = in->display == OTA_DISPLAY_SKIPPED || (in->display == OTA_DISPLAY_UP && in->shell_up);
    return in->uptime_s >= OTA_VALID_AFTER_S && in->c6_up && !in->psram_trial && ui_ok;
}

ota_trial_act_t ota_trial_step(ota_trial_in_t const *in) {
    if (in->image == OTA_IMAGE_UNKNOWN) {
        return OTA_TRIAL_UNKNOWN;
    }
    if (in->image != OTA_IMAGE_ON_TRIAL) {
        return OTA_TRIAL_DONE;
    }
    if (proven(in)) {
        return OTA_TRIAL_MARK_VALID;
    }
    if (ota_should_roll_back_now(true, in->c6_up, in->uptime_s)) {
        return OTA_TRIAL_ROLL_BACK;
    }
    if (!in->c6_up || in->psram_trial || in->uptime_s < OTA_VALID_AFTER_S) {
        return OTA_TRIAL_WAIT; // the C6 link down is watched up to the rollback (#295)
    }
    return OTA_TRIAL_STAY;
}

ota_trial_end_t ota_trial_run(ota_trial_io_t const *io) {
    unsigned read_fails = 0, mark_fails = 0;
    for (;;) {
        ota_trial_in_t in = {0};
        io->read(io->ctx, &in);
        switch (ota_trial_step(&in)) {
        case OTA_TRIAL_DONE:
            return OTA_TRIAL_END_NOT_ON_TRIAL;
        case OTA_TRIAL_UNKNOWN:
            if (++read_fails >= OTA_TRIAL_TRIES) {
                return OTA_TRIAL_END_NO_STATE;
            }
            break;
        case OTA_TRIAL_MARK_VALID:
            if (io->mark_valid(io->ctx)) {
                return OTA_TRIAL_END_VALID;
            }
            if (++mark_fails >= OTA_TRIAL_TRIES) {
                return OTA_TRIAL_END_MARK_FAILED;
            }
            break;
        case OTA_TRIAL_ROLL_BACK:
            return OTA_TRIAL_END_ROLL_BACK;
        case OTA_TRIAL_STAY:
            return OTA_TRIAL_END_STAY;
        case OTA_TRIAL_WAIT:
        default:
            break;
        }
        io->wait(io->ctx);
    }
}

bool ota_should_roll_back_now(bool pending_verify, bool c6_up, uint32_t uptime_s) {
    return pending_verify && !c6_up && uptime_s >= OTA_VALID_AFTER_S + OTA_ROLLBACK_GRACE_S;
}

bool ota_image_header_ok(uint8_t const *head, size_t len, uint16_t chip_id) {
    // magic 0xE9 at 0, chip id little-endian at 12 (esp_app_format.h)
    return head && len >= OTA_IMAGE_HEADER_LEN && head[0] == 0xE9 &&
           (uint16_t)(head[12] | (uint16_t)head[13] << 8) == chip_id;
}
