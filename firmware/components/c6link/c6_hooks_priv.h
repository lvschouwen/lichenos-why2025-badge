#pragma once

#include "c6_hooks.h"

// c6link's own side of the hook table (c6_hooks.h, #223), not for other components: calling attached from outside
// would bring Wi-Fi up a second time. Each call falls back to the default its member names when the hook is unset.
void     c6_hooks_step_begin(void);
void     c6_hooks_step_progress(int percent);
void     c6_hooks_step_end(bool ok, char const *note);
bool     c6_hooks_image_on_trial(void); // true when unset: the cautious answer
bool     c6_hooks_attached(void); // false when nobody took the link (no net service)
void     c6_hooks_no_link(void);
bool     c6_hooks_driver_down(uint32_t wait_ms);
bool     c6_hooks_driver_up(uint32_t wait_ms);
unsigned c6_hooks_stack_free(void);
