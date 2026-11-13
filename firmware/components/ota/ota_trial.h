#pragma once

#include "esp_partition.h"
#include "ota_valid.h"

#include <stdbool.h>

// The one owner of "is this image on trial" (#285): every other component asks here (the shell through
// lichen_sys_update.h; the layers below ota through a hook main binds) instead of reading the OTA data itself.
// tools/layers.conf makes esp_ota_get_state_partition, esp_ota_get_last_invalid_partition and
// esp_ota_mark_app_valid_cancel_rollback ota's only.

// The running image's trial state: ON_TRIAL (PENDING_VERIFY) means a reset before it is marked valid makes the
// bootloader boot the previous image again. Each read of the OTA data maps a flash page, so the answer is kept once
// it is known: only ota_image_mark_valid changes it while the image runs. UNKNOWN when the read failed (not kept: the
// next call reads again). Any task.
ota_image_state_t ota_image_trial_state(void);
// What ota_image_trial_state already knows, without reading the OTA data: UNKNOWN until a read succeeded. For a
// restart path that must not touch flash (__wrap_esp_restart's warning, #304). Any task.
ota_image_state_t ota_image_trial_known(void);
// ota_image_trial_state() == OTA_IMAGE_ON_TRIAL: a failed read counts as not on trial (as before #285). Any task.
bool ota_image_trial_pending(void);
// ota_image_may_be_on_trial(ota_image_trial_state()): ON_TRIAL or UNKNOWN. For callers that may restart (psram_speed,
// c6link): a restart on a state that could not be read might roll the image back. Any task.
bool ota_image_trial_maybe(void);
// Cancels the rollback: the image on trial is the one the bootloader keeps. False when it could not be saved (the image
// then stays on trial). Called by main's trial (ota_valid.h decides when). Not from an interrupt.
bool ota_image_mark_valid(void);
// The bootloader marked an image invalid (a trial that failed) and that is not the one running: this boot went back to
// the previous image. Any task.
bool ota_rolled_back(void);
// How this image's trial started (#411): the arrival mark is taken once per boot, on the first call of this,
// ota_image_note_slot_switch or the test arm, and kept in RAM afterwards. Any task.
ota_arrival_t ota_image_arrival(void);
// The developer-mode test arm was set before this boot: its trial counts Wi-Fi as not rejoined. Any task.
bool ota_image_arrival_test_no_wifi(void);
// `fw slot other` made slot the boot slot (only then; fw_console calls it through console_bind_slot_switch): the next
// boot, if it boots that slot, keeps the trial rule as before. Not from an interrupt.
void ota_image_note_slot_switch(esp_partition_t const *slot);
// Registers the developer-mode `trial` command (compiled out of release builds). Once, at start.
void ota_trial_register_cmds(void);
// "fw slot": the OTA state of an app slot ("new", "pending-verify", "valid", "invalid", "aborted", "undefined", or
// "unknown" when there is no slot or nothing could be read). Any task.
char const *ota_slot_state_name(esp_partition_t const *p);
