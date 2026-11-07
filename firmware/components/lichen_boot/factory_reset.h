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
// main.c, right before storage_mount: for a pending "everything", erases the storage partition from flash (the C6
// bundle and lora.toml are carried over in PSRAM) and formats it. Up to ~30 s with the screen still dark.
void factory_reset_boot_storage(void);
// main.c, right after lichen_notify_start: an error notice when this boot's reset could not erase from flash
void factory_reset_post_notices(void);
// How those notices are raised (#245: lichen_boot does not include the gate's lichen_notify.h): lichen_os binds an
// error notice from source "lichen" in start_nvs. Unbound, the notice only goes to the log.
void factory_reset_bind_notice(void (*error)(char const *title, char const *body));
// "fw factory-reset": plan (a dry run, erases nothing) | settings YES | all YES
void factory_reset_console_register(void);
