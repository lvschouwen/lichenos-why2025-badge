#pragma once
// The OS video surface's owner record (#561, part of #527): who holds the scaled video rectangle, the two source buffers,
// and the calls the app-facing API (lichen_video.h, #562) and the loader will make. Internal: no capability, no app sees it.
// Every call may be made from any task but the LVGL task's timers and takes the UI service's lock (bounded); nothing holds it
// across frames. The sources are plain PSRAM from the heap (not lichen_alloc): never in an app's block book or the loader's
// sweep maps, so the unload sweep cannot free them; video_owner_release_ctx frees them, before that sweep.

#include "display_video_rules.h"
#include "display_video_state.h"
#include "lichen_app.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>

// Called under the LVGL lock when the screen is taken back (a reason that is not the app's): no blocking. It may call
// video_owner_release: the UI lock is recursive, the same task re-enters it, and the display side tolerates the state
// change (the timer is already stopped, the reclaim then finds IDLE) - not tested on the badge.
typedef void (*video_reclaim_fn)(void *arg, display_video_reclaim_t why);

// Takes the surface for ctx: allocates two w x h RGB565 sources (black, 64-byte aligned PSRAM) into src[0..1] and hands the
// screen over. The app writes its first frame into src[1]: src[0] is the black one the surface shows until the first
// present, so nobody may write src[0] before the second present (then the two alternate). NONE or the refusal: owned, no PPA engine (cpu fallback, hung), PSRAM at 80 MHz, geometry, memory, UI lock.
display_video_refusal_t video_owner_acquire(lichen_ctx_t ctx, uint32_t w, uint32_t h, uint32_t want_x_milli,
                                            uint32_t want_y_milli, video_reclaim_fn on_reclaim, void *arg, void *src[2]);
// Source idx (0/1) is finished: show it. Lock-free. False when ctx does not hold the surface, it was reclaimed, or dropped.
bool video_owner_present(lichen_ctx_t ctx, unsigned idx);
// Lets go (idempotent). True when done; false only when the UI lock was not free in time: nothing was released.
bool video_owner_release(lichen_ctx_t ctx);
// The loader's hook, before an app's image and sweeps go: releases what ctx holds. release_app_holdings runs after the
// unload steps stopped the app's tasks and timers (sd_app_loader.c unload(), and the card-removal path), so no present() of
// the app is in flight; a late one is refused by its state check. False = lock timeout, keep the image
// (the reclaim callback may point into it).
bool video_owner_release_ctx(lichen_ctx_t ctx);
// True while ctx holds the record (acquired, not released, not swept by an unload), reclaimed or not: its sources are
// still valid. Lock-free, like present.
bool video_owner_holds(lichen_ctx_t ctx);
// An explicit event takes the screen back (dialog, thermal, card, hold-Esc). False when it is not ACTIVE.
bool video_owner_reclaim(display_video_reclaim_t why);

#if CONFIG_LICHEN_DEV_COMMANDS
typedef struct {
    bool                    held; // an owner record exists (it outlives a reclaim until the app releases)
    bool                    reclaimed;
    bool                    parked; // sources kept because the PPA may still read them
    lichen_ctx_t            ctx;
    display_video_state_t   state;
    display_video_reclaim_t reason;
    uint32_t                acquires, reclaims, presented, frames_dropped, strips_dropped, bands_lost, present_max_us, queued;
} video_owner_stats_t;
void video_owner_stats(video_owner_stats_t *out);
void video_owner_register(void); // "fw video" (developer mode)
#else
static inline void video_owner_register(void) {
}
#endif
