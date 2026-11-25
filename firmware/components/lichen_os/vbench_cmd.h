#pragma once
// "fw vbench" (#558 spike), developer builds only: registered by disp_state_start, nothing in a release image.
#include "sdkconfig.h"

#if CONFIG_LICHEN_DISPLAY_DIRECT_FLUSH && CONFIG_LICHEN_DEV_COMMANDS
void vbench_cmd_register(void);
#else
static inline void vbench_cmd_register(void) {
}
#endif
