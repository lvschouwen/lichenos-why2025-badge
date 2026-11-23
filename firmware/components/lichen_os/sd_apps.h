#pragma once
// Apps from the SD card (#516). sd_app_loader.c is the service (#523): a tile on Home for every app of /sd/lichen/apps, the
// ELF loaded when its tile is opened and unloaded when its last screen is closed or the card goes. The spike (#517), the
// developer-mode command "fw elf run <path>", is sd_apps.c.

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

// Resolves an app's undefined symbols against the generated export table (lichen_exports.c, #519): once, from either
// caller; later calls do nothing. Any task.
void sd_apps_register_symbols(void);
// The boot step START_SD_APPS (main task, once, after the apps are registered and the app list is live): starts the
// loader's task and watches the card. Nothing starts on a degraded boot (safe mode, #232): no SD app runs then.
void sd_apps_boot(void);

// Registers "fw elf" (developer mode only). A release build (LICHEN_DEV_COMMANDS off) has no sd_apps.c: an empty inline.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
void sd_apps_console_register(void);
#else
static inline void sd_apps_console_register(void) {
}
#endif

#ifdef __cplusplus
}
#endif
