#pragma once
// Tile badges (#102): a count per app that home draws as a red circle with the number in white on the top-right corner
// of the app's icon, in both home layouts ("99+" above 99, nothing at 0). The OS keeps the counts; an app (or the OS
// for it) sets its own whenever it changes: the crash log its unseen crashes, MeshCore its unread messages. The
// notifications service (#62) sets the badge of every app that raises notices to its unread count: such an app must
// not also set its own badge here (the two would overwrite each other; MeshCore moves over in #50). Any task (a
// spinlock, held only to copy a number): never from an ISR.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The count for the registered app with this id; 0 clears it. An id no registered app has is ignored.
void     lichen_badge_set(char const *app_id, unsigned count);
unsigned lichen_badge_get(char const *app_id); // 0 when none is set
// Changes whenever a count changes (not when it is set to what it was): home redraws its badges then
uint32_t lichen_badge_generation(void);

#ifdef __cplusplus
}
#endif
