#pragma once

#include "sdkconfig.h"

// "fw soak" (#409, soak_line.h): developer mode only. A release build (LICHEN_DEV_COMMANDS off) has no command.
#ifdef CONFIG_LICHEN_DEV_COMMANDS
void soak_cmd_register(void);
#else
static inline void soak_cmd_register(void) {
}
#endif
