#pragma once
// The notification service's own entry for a source it names itself (#230 E1c): lichen_notify_to takes the source from
// the caller's ctx, so the console's "fw notify test" (a source no app has) posts through here. Inside lichen_notify:
// not a gate header, no app or shell code includes it.

#include "lichen_notify.h"

#ifdef __cplusplus
extern "C" {
#endif

// As lichen_notify_to, under source (not NULL, at most an app id long) instead of a ctx's
uint32_t lichen_notify_post(char const *source, ln_level_t level, char const *target, char const *title,
                            char const *body, uint32_t flags);

#ifdef __cplusplus
}
#endif
