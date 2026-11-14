// ota_trial.h: the image's trial state, read from the OTA data in one place (#285)
#include "ota_trial.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "c6link.h"
#include "fw_console.h"
#include "ota_hold.h"
#include "freertos/FreeRTOS.h"

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

static char const TAG[] = "ota";

static _Atomic int trial_known = OTA_IMAGE_UNKNOWN; // an ota_image_state_t

// The arrival mark (#411, ota_valid.h): LP RAM, which #330 measured to survive esp_restart and a panic and to be lost
// at a power-off and the EN reset. Only this file writes it, only for `fw slot other` and the dev test arm.
static RTC_NOINIT_ATTR ota_arrival_mark_t s_mark;
static portMUX_TYPE        s_mark_lock = portMUX_INITIALIZER_UNLOCKED;
static bool                s_taken;    // under s_mark_lock
static ota_arrival_taken_t s_arrival;  // under s_mark_lock once s_taken

// Takes the mark once, before anything this boot may write a new one (lock held by the caller)
static void take_locked(uint32_t running_addr) {
    if (!s_taken) {
        s_arrival = ota_arrival_take(&s_mark, running_addr);
        s_taken   = true;
    }
}

static uint32_t running_addr(void) {
    esp_partition_t const *run = esp_ota_get_running_partition(); // outside the spinlock: it may read flash once
    return run ? run->address : UINT32_MAX;
}

static ota_arrival_taken_t arrival(void) {
    uint32_t addr = running_addr();
    portENTER_CRITICAL(&s_mark_lock);
    take_locked(addr);
    ota_arrival_taken_t t = s_arrival;
    portEXIT_CRITICAL(&s_mark_lock);
    return t;
}

ota_arrival_t ota_image_arrival(void) {
    return arrival().arrival;
}

bool ota_image_arrival_test_no_wifi(void) {
    return arrival().test_no_wifi;
}

void ota_image_note_slot_switch(esp_partition_t const *slot) {
    uint32_t addr = running_addr();
    portENTER_CRITICAL(&s_mark_lock);
    take_locked(addr); // this boot's own mark first: the new one is for the next boot
    ota_arrival_mark_switch(&s_mark, slot->address);
    portEXIT_CRITICAL(&s_mark_lock);
    ESP_LOGI(TAG, "%s chosen at the console: its trial needs no Wi-Fi", slot->label);
}

#ifdef CONFIG_LICHEN_DEV_COMMANDS
// `fw trial nowifi` (developer mode): the next boot's trial counts Wi-Fi as not rejoined, so an image installed over
// Wi-Fi rolls back at OTA_VALID_NEEDS_WIFI_S. Proves the rollback without touching an access point; it can only make a
// trial stricter, and a software restart is what carries it (a power-off loses it).
static void cmd_trial(char const *args) {
    bool arm = strcmp(args, "nowifi") == 0;
    if (!arm && strcmp(args, "off") != 0) {
        console_reply("trial <nowifi|off>: the next boot's trial ignores Wi-Fi (an OTA image rolls back), or not");
        return;
    }
    uint32_t addr = running_addr();
    portENTER_CRITICAL(&s_mark_lock);
    take_locked(addr);
    ota_arrival_arm_test(&s_mark, arm);
    portEXIT_CRITICAL(&s_mark_lock);
    if (arm) {
        console_reply("trial: armed, an image installed over Wi-Fi now rolls back %d s after its boot (one boot)",
                      OTA_VALID_NEEDS_WIFI_S);
    } else {
        console_reply("trial: test arm cleared");
    }
}
#endif

void ota_trial_register_cmds(void) {
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    console_register_dev("trial", "trial <nowifi|off> (dev: the next boot's trial ignores Wi-Fi, #411)",
                         cmd_trial);
#endif
}

char const *ota_boot_pending(void) {
    // The loads of the Dekker pair (ota_hold.h): seq_cst, after the caller's store of onair_on
    return ota_pending_why(ota_change_busy(), ota_change_installed(), ota_image_trial_state(), c6link_bundle_pending());
}

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

ota_image_state_t ota_image_trial_known(void) {
    return (ota_image_state_t)atomic_load(&trial_known);
}

bool ota_image_trial_pending(void) {
    return ota_image_trial_state() == OTA_IMAGE_ON_TRIAL;
}

bool ota_image_trial_maybe(void) {
    return ota_image_may_be_on_trial(ota_image_trial_state());
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
