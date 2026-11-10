// ota_trial.h: the image's trial state, read from the OTA data in one place (#285)
#include "ota_trial.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

#include <stdatomic.h>

static char const TAG[] = "ota";

static _Atomic int trial_known = OTA_IMAGE_UNKNOWN; // an ota_image_state_t

ota_image_state_t ota_image_trial_state(void) {
    int known = atomic_load(&trial_known);
    if (known != OTA_IMAGE_UNKNOWN) {
        return (ota_image_state_t)known;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) {
        return OTA_IMAGE_UNKNOWN; // not kept, not logged (the PSRAM tick asks every second): the next call reads again
    }
    // Only from UNKNOWN: a mark that landed in between (NOT_ON_TRIAL) is never undone by this older read
    int expected = OTA_IMAGE_UNKNOWN;
    atomic_compare_exchange_strong(&trial_known, &expected,
                                   state == ESP_OTA_IMG_PENDING_VERIFY ? OTA_IMAGE_ON_TRIAL : OTA_IMAGE_NOT_ON_TRIAL);
    return (ota_image_state_t)atomic_load(&trial_known);
}

bool ota_image_trial_pending(void) {
    return ota_image_trial_state() == OTA_IMAGE_ON_TRIAL;
}

bool ota_image_mark_valid(void) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marking this image valid failed: %s", esp_err_to_name(err));
        return false;
    }
    atomic_store(&trial_known, OTA_IMAGE_NOT_ON_TRIAL);
    return true;
}

bool ota_rolled_back(void) {
    esp_partition_t const *invalid = esp_ota_get_last_invalid_partition();
    return invalid && invalid != esp_ota_get_running_partition();
}

char const *ota_slot_state_name(esp_partition_t const *p) {
    esp_ota_img_states_t state;
    if (!p || esp_ota_get_state_partition(p, &state) != ESP_OK) {
        return "unknown";
    }
    switch (state) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}
