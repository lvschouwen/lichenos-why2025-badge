#pragma once

#include "display_rot.h"

#include <stdbool.h>
#include <stdint.h>

// Pure owner state machine and flip rule of the OS video surface (#559, part of #527); host_tests/
// display_video_state_test.c. No ESP-IDF, no LVGL, no locking: the caller runs it on the LVGL task.

typedef enum {
    DISPLAY_VIDEO_IDLE,       // LVGL draws everything
    DISPLAY_VIDEO_ACTIVE,     // an app owns the video rectangle; LVGL draws the bands only
    DISPLAY_VIDEO_RECLAIMING, // the screen is being handed back (callback, invalidate); no frames
} display_video_state_t;

// Why the screen is taken back. Only these end ACTIVE against the app's will; a strip landing in the video
// rectangle is not one of them.
typedef enum {
    DISPLAY_VIDEO_RECLAIM_NONE,
    DISPLAY_VIDEO_RECLAIM_DIALOG,  // a critical dialog
    DISPLAY_VIDEO_RECLAIM_THERMAL, // thermal warning
    DISPLAY_VIDEO_RECLAIM_CARD,    // the SD card was removed
    DISPLAY_VIDEO_RECLAIM_ESCAPE,  // hold-Esc
    DISPLAY_VIDEO_RECLAIM_ENGINE,  // the PPA engine was lost
    DISPLAY_VIDEO_RECLAIM_COUNT,
} display_video_reclaim_t;

typedef struct {
    display_video_state_t   state;
    display_video_reclaim_t reason; // of the current or last reclaim
    uint32_t                acquires, reclaims, refused;
    uint32_t                band_last; // band strips the last present wrote: only in that buffer, the other lacks them
} display_video_owner_t;

void display_video_owner_init(display_video_owner_t *o);

// IDLE -> ACTIVE. Refused (false, counted) in any other state.
bool display_video_acquire(display_video_owner_t *o);
// ACTIVE -> RECLAIMING with a reason (not NONE). Refused in IDLE and RECLAIMING, and for an invalid reason.
bool display_video_reclaim(display_video_owner_t *o, display_video_reclaim_t why);
// RECLAIMING -> IDLE once the callback ran and the screen is invalidated. False elsewhere; only a call while ACTIVE
// counts as refused (after a release it is the harmless race of the app letting go mid-reclaim).
bool display_video_reclaim_done(display_video_owner_t *o);
// Any state -> IDLE (the app released, or was unloaded); clears the reclaim reason, and the reclaim_done that
// would have followed is then simply not needed. True when the owner was not IDLE.
bool display_video_release(display_video_owner_t *o);

char const *display_video_state_name(display_video_state_t s);
char const *display_video_reclaim_name(display_video_reclaim_t r);

// Flip rule. While ACTIVE, strips outside the video (the status bar) are held and ride the next video present:
// they never trigger a buffer swap of their own, so no stale video frame is shown and no 777 KB carry copy is needed.
// With two buffers and no dirty-carry of the video, the bands a present wrote are only in that buffer: the next present
// first copies them into the new back buffer (`carry`, band-sized), else the status bar flickers between versions.
// Outside ACTIVE every strip goes out at once and the frame ends as usual (the flip's own dirty list carries).
typedef struct {
    uint32_t take;  // how many of the pending band strips go into this frame (the first `take`)
    uint32_t carry; // how many band strips of the previous present to copy into this back buffer first
    bool     swap;  // request the buffer swap at the end of this frame
    bool     video; // the video frame is drawn into this frame
} display_video_plan_t;

// `pending` band strips are waiting; `present` = a video frame is ready to be drawn. Records what this frame wrote
// for the next call's carry.
display_video_plan_t display_video_plan(display_video_owner_t *o, uint32_t pending, bool present);
