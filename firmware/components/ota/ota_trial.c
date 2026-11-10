// ota_trial.h: the image's trial state, read from the OTA data in one place (#285)
#include "ota_trial.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

#include <stdatomic.h>

static char const TAG[] = "ota";

typedef enum { TRIAL_UNKNOWN, TRIAL_PENDING, TRIAL_NOT } trial_known_t;

static _Atomic int trial_known = TRIAL_UNKNOWN;

bool ota_image_trial_pending(void) {
    int known = atomic_load(&trial_known);
    if (known != TRIAL_UNKNOWN) {
        return known == TRIAL_PENDING;
    }
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) {
        return false; // not kept: the next call reads again
    }
    bool pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    // Only from UNKNOWN: a mark that landed in between (TRIAL_NOT) is never undone by this older read
    int expected = TRIAL_UNKNOWN;
    atomic_compare_exchange_strong(&trial_known, &expected, pending ? TRIAL_PENDING : TRIAL_NOT);
    return atomic_load(&trial_known) == TRIAL_PENDING;
}

bool ota_image_mark_valid(void) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "marking this image valid failed: %s", esp_err_to_name(err));
        return false;
    }
    atomic_store(&trial_known, TRIAL_NOT);
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
