#pragma once
// Apps from the SD card (#516): the native ELF loader's glue. The spike (#517) is one developer-mode command,
// "fw elf run <path>"; the loader service, the mount scan and unload on card removal come later (#516).

#ifdef __cplusplus
extern "C" {
#endif

#include "sdkconfig.h"

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
