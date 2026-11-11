#pragma once

// The OS's own notices (#230 E1c): raised as the source "lichen" (gate_app.h's GATE_OS_SOURCE). lichen_notify.h's calls
// take the source from the caller's app ctx and refuse the OS's ctx, which an app can reach through lichen_ctx_self()
// outside its hooks, so the OS (thermal, system notices, the factory reset, the shell) raises through these instead.
// tools/layers.conf forbids lichen_sys_* in L6. Same rules as lichen_notify.h: any task, never from an ISR.

#include "lichen_notify.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t lichen_sys_notify(ln_level_t level, char const *title, char const *body, uint32_t flags);
uint32_t lichen_sys_notify_to(ln_level_t level, char const *target, char const *title, char const *body,
                              uint32_t flags);

#ifdef __cplusplus
}
#endif
