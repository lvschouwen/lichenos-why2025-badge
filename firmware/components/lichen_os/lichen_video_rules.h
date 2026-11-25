#pragma once
// The decisions behind lichen_video.h (#562) without the ESP: who may call, what the app's frame cycle does with the
// two sources, and what a stale record means. lichen_video.c binds the ops to the capability check and video_owner.h.

#include "display_video_rules.h"
#include "display_video_state.h"
#include "lichen_video.h"

#include <stdbool.h>
#include <stdint.h>

// Geometry is the OS's (#527): a 320 x 200 source at these scales is 720 x 537, centred. Other sizes go to the display's
// own fit check, so a source that does not fit is refused, not stretched.
_Static_assert((int)LICHEN_VIDEO_RECLAIM_DIALOG == (int)DISPLAY_VIDEO_RECLAIM_DIALOG &&
                   (int)LICHEN_VIDEO_RECLAIM_ENGINE == (int)DISPLAY_VIDEO_RECLAIM_ENGINE &&
                   (int)DISPLAY_VIDEO_RECLAIM_COUNT == (int)LICHEN_VIDEO_RECLAIM_ENGINE + 1,
               "lichen_video_reclaim_t follows display_video_reclaim_t");

#define LICHEN_VIDEO_SCALE_X_MILLI 2250u
#define LICHEN_VIDEO_SCALE_Y_MILLI 2700u

typedef struct {
    bool (*cap_ok)(void);   // the calling app declared VIDEO (the OS and SYSTEM apps pass)
    uint32_t (*self)(void); // the caller's context
    display_video_refusal_t (*acquire)(uint32_t ctx, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy,
                                       void (*on_reclaim)(void *, display_video_reclaim_t), void *arg, void *src[2]);
    bool (*present)(uint32_t ctx, unsigned idx);
    bool (*release)(uint32_t ctx);
    bool (*holds)(uint32_t ctx); // the owner record is still this ctx's (not released, not swept by an unload)
} lichen_video_ops_t;

typedef struct {
    uint32_t                ctx; // the holder: acquire binds it, so present works from any task of the app
    void                   *src[2];
    unsigned                back; // the source the next frame goes into
    lichen_video_reclaim_fn cb;
    void                   *cb_arg;
} lichen_video_t;

bool      lichen_video_do_acquire(lichen_video_t *v, lichen_video_ops_t const *ops, lichen_video_cfg_t const *cfg,
                                  void (*on_reclaim)(void *, display_video_reclaim_t));
uint16_t *lichen_video_do_back(lichen_video_t *v, lichen_video_ops_t const *ops);
bool      lichen_video_do_present(lichen_video_t *v, lichen_video_ops_t const *ops);
bool      lichen_video_do_release(lichen_video_t *v, lichen_video_ops_t const *ops);
// The owner's reclaim callback, forwarded to the app's (mapped to the app-facing enum)
void lichen_video_do_reclaimed(lichen_video_t const *v, display_video_reclaim_t why);
