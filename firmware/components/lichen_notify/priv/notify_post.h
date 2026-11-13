#pragma once
// The console's own notices (#230 E1c, #340): lichen_notify_to takes the source from the caller's ctx, and "fw notify
// test" raises under "test", a source no app can have (gate_app.h). Private to lichen_notify (its PRIV_INCLUDE_DIRS):
// no gate header, no app or shell code reaches it, and it posts under "test" only, never a source the caller names.

#include "lichen_notify.h"

#ifdef __cplusplus
extern "C" {
#endif

// As lichen_notify_to, under source "test"
uint32_t lichen_notify_post_test(ln_level_t level, char const *target, char const *title, char const *body,
                                 uint32_t flags);

#ifdef __cplusplus
}
#endif
