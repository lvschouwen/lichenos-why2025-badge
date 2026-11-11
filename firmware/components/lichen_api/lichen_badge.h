#pragma once
// Tile badges (#102): a count per app that home draws as a red circle with the number in white on the top-right corner
// of the app's icon, in both home layouts ("99+" above 99, nothing at 0). The OS keeps the counts; an app (or the OS
// for it) sets its own whenever it changes: the crash log its unseen crashes, MeshCore its unread messages. The
// notifications service (#62) does not touch badges: an app that raises notices sets its own count here, ideally the
// same number it shows elsewhere (MeshCore: its unread chats that notify, #50). Any task (a
// spinlock, held only to copy a number): never from an ISR.

#include "lichen_app.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The caller's own tile count (#230 E1c): ctx is the one lichen_register_app gave the app, and an app cannot reach
// another's tile. 0 clears it. A stale or made-up ctx, the OS's (it has no tile) or an app without an id does nothing
// (a refused ctx is logged at most once per 10 s and counted in "fw app ctx", never an abort); get gives 0 then.
// What the OS sets or reads for any app's tile is lichen_sys_badge.h.
void     lichen_badge_set(lichen_ctx_t ctx, unsigned count);
unsigned lichen_badge_get(lichen_ctx_t ctx); // 0 when none is set
// Changes whenever a count changes (not when it is set to what it was): home redraws its badges then
uint32_t lichen_badge_generation(void);

#ifdef __cplusplus
}
#endif
