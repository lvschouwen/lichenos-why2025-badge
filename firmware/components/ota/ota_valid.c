#include "ota_valid.h"

bool ota_should_mark_valid(bool pending_verify, bool ui_up, bool c6_up, uint32_t uptime_s) {
    return pending_verify && ui_up && c6_up && uptime_s >= OTA_VALID_AFTER_S;
}
