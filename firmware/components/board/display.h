#pragma once

#include "lvgl.h"

#include "display_info.h"
#include "display_video_rules.h"
#include "display_video_state.h"

#include <stdbool.h>
#include <stdint.h>

// The 720x720 panel driven through LVGL (esp_lvgl_port); draw buffers in PSRAM (#61)
bool display_start(lv_display_t **out);

// Switches the panel off (display-off command; a later display_start() resets and re-inits it). Safe from a shutdown
// handler; does nothing before display_start() succeeded.
void display_blank(void);


// The OS video surface (#561, part of #527): an app (through lichen_os/video_owner.c) owns a scaled rectangle of the panel;
// LVGL draws only the bars around it. Every *_locked call runs under the LVGL lock (the UI service's, taken by the caller)
// and none keeps it across frames. Without the direct flush all of them refuse.
typedef void (*display_vsurf_reclaim_fn)(void *arg, display_video_reclaim_t why);

typedef struct {
    display_video_state_t   state;
    display_video_reclaim_t reason;
    uint32_t                acquires, reclaims, presented;
    uint32_t                frames_dropped; // a present that found the slot taken, or a frame the pump could not draw
    uint32_t                strips_dropped; // LVGL strips entirely inside the video rectangle
    uint32_t                bands_lost;     // band strips that did not fit the band buffer (re-rendered by LVGL)
    uint32_t                present_max_us; // one pump tick: the band strips and the scaled frame, PPA done included
    uint32_t                queued;         // band strips waiting for the next present
} display_vsurf_stats_t;

// IDLE -> ACTIVE for a w x h RGB565 source (src[0], src[1]: 64-byte aligned PSRAM, written black or with a picture, size
// w * h * 2 and a multiple of 64) at the wanted scales in thousandths. Paints the bars black in both frame buffers and
// invalidates the screen, so the bars come back through the band queue. on_reclaim(arg, why) runs under the lock when the
// screen is taken back; it must not block or take the lock. NONE or the first refusal.
display_video_refusal_t display_vsurf_acquire_locked(void *const src[2], uint32_t w, uint32_t h, uint32_t want_x_milli,
                                                     uint32_t want_y_milli, unsigned psram_mhz,
                                                     display_vsurf_reclaim_fn on_reclaim, void *arg);

// Hands the surface back (the app released, or its image is going): IDLE, band queue dropped, screen invalidated. True when
// the sources and the band buffer are no longer read by the PPA (waited out, bounded); false: a transfer may still run, the
// caller keeps the sources (display_vsurf_ppa_idle_locked says when they may go). Idempotent.
bool display_vsurf_release_locked(void);
bool display_vsurf_ppa_idle_locked(void);

// The screen is taken back for a reason that is not the app's: on_reclaim is called, the screen invalidated, state IDLE.
// The owner record stays with the app until it releases. False when not ACTIVE.
bool display_vsurf_reclaim_locked(display_video_reclaim_t why);

// A finished source buffer is ready to be shown. Any task, no lock: writes the buffer back, hands it to the LVGL task (one
// slot, waits up to 100 ms for the previous frame to be drawn) and wakes it. False: dropped (not ACTIVE, or the slot stayed
// taken). The buffer is the OS's until the next present call returns.
bool display_vsurf_present(unsigned idx);

void display_vsurf_stats(display_vsurf_stats_t *out); // any task, a racy read
