#pragma once

#include "esp_partition.h"

#include <stdbool.h>

// The one owner of "is this image on trial" (#285): every other component asks here (the shell through
// lichen_sys_update.h; the layers below ota through a hook main binds) instead of reading the OTA data itself.
// tools/layers.conf makes esp_ota_get_state_partition, esp_ota_get_last_invalid_partition and
// esp_ota_mark_app_valid_cancel_rollback ota's only.

// The running image is on trial (PENDING_VERIFY): a reset before it is marked valid makes the bootloader boot the
// previous image again. Each read of the OTA data maps a flash page, so the answer is kept once it is known: only
// ota_image_mark_valid changes it while the image runs. A failed read counts as not on trial (as before #285) and is
// tried again next time. Any task.
bool ota_image_trial_pending(void);
// Cancels the rollback: the image on trial is the one the bootloader keeps. False when it could not be saved (the image
// then stays on trial). Called by main's trial (ota_valid.h decides when). Not from an interrupt.
bool ota_image_mark_valid(void);
// The bootloader marked an image invalid (a trial that failed) and that is not the one running: this boot went back to
// the previous image. Any task.
bool ota_rolled_back(void);
// "fw slot": the OTA state of an app slot ("new", "pending-verify", "valid", "invalid", "aborted", "undefined", or
// "unknown" when there is no slot or nothing could be read). Any task.
char const *ota_slot_state_name(esp_partition_t const *p);
