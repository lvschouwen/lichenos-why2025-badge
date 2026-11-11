#pragma once

// The OS's side of the tile badges (#230 E1c): set or read the count of any app's tile by its id, for the shell (home
// draws every app's badge, the notification service's unread counts go to the apps' tiles) and for nobody else:
// tools/layers.conf forbids lichen_sys_* in L6. An app sets and reads its own tile with lichen_badge.h.

#ifdef __cplusplus
extern "C" {
#endif

// The count for the registered app with this id; 0 clears it. An id no registered app has is ignored.
void     lichen_sys_badge_set(char const *app_id, unsigned count);
unsigned lichen_sys_badge_get(char const *app_id); // 0 when none is set

#ifdef __cplusplus
}
#endif
