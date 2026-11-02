#pragma once

#include "factory_plan.h"

#include <stdbool.h>

// Factory reset (#66), the OS service. A reset is asked for (Settings > Factory reset, the boot combo, "fw
// factory-reset") by saving it as pending and restarting; the next boot does it before anything else reads NVS or the
// drive. What each level erases: factory_plan.h. Apps have no access to any of this.

// Saves level as pending; the caller restarts the badge. False when NVS refused (nothing will be erased).
bool factory_reset_request(factory_level_t level);
// main.c, right after nvs_flash_init: erases NVS for a pending reset
void factory_reset_boot_nvs(void);
// main.c, right after storage_mount (storage_ok: it mounted): wipes /storage for a pending "everything". A drive that
// did not mount is formatted then. Returns whether /storage is mounted afterwards.
bool factory_reset_boot_storage(bool storage_ok);
// "fw factory-reset": plan (a dry run, erases nothing) | settings YES | all YES
void factory_reset_console_register(void);
