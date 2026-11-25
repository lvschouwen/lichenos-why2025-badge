#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pure decisions of the OS video surface (#561, part of #527); host_tests/display_video_rules_test.c: why an acquire is
// refused, and which source buffer a pump tick presents.

typedef enum {
    DISPLAY_VIDEO_REFUSE_NONE,
    DISPLAY_VIDEO_REFUSE_OWNED,  // another owner holds the surface (or it is being handed back)
    DISPLAY_VIDEO_REFUSE_ENGINE, // no PPA: the engine is on the CPU, hung, not set up, or there is no second frame buffer
    DISPLAY_VIDEO_REFUSE_PSRAM,  // PSRAM at 80 MHz: the PPA's reads and the panel's scan-out do not both fit
    DISPLAY_VIDEO_REFUSE_GEOM,   // the source does not fit the panel at the wanted scale
    DISPLAY_VIDEO_REFUSE_MEM,    // a buffer did not allocate
    DISPLAY_VIDEO_REFUSE_UI,     // the LVGL lock was not free in time (video_owner.c: nothing was tried)
} display_video_refusal_t;

#define DISPLAY_VIDEO_PSRAM_MIN_MHZ 120

typedef struct {
    bool     owned;       // the surface is not IDLE
    bool     engine_ppa;  // the active engine is a PPA one and its client is up
    bool     engine_hung; // a transfer timed out and has not reported its done
    bool     swap_on;     // two frame buffers
    unsigned psram_mhz;
    bool     geom_ok;
    bool     mem_ok;
} display_video_acquire_in_t;

// First failing rule, in the order of the enum; NONE when the acquire may go ahead
display_video_refusal_t display_video_acquire_verdict(display_video_acquire_in_t const *in);
char const             *display_video_refusal_name(display_video_refusal_t r);

// A source buffer is shown again (nothing new was presented) when band strips wait this long, so the status bar stays live
// while an app is paused
#define DISPLAY_VIDEO_KEEPALIVE_US 100000u

// Which source one pump tick presents: `pending` (< 0: none) is the app's new frame; else, with band strips waiting, the
// last frame again once `since_last_us` reaches the keep-alive (at once when there was none yet: `last` < 0 means the
// first, black, source 0). -1 = nothing to do.
int display_video_pump_pick(int pending, int last, uint32_t bands, uint32_t since_last_us);
