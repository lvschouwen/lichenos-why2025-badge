#pragma once

#include "display_rot.h"

#include <stdbool.h>
#include <stdint.h>

// Pure half of the tear-free flush (#467): two DSI frame buffers, the strips of a frame go into the one that is not
// scanned out, the switch is requested at the end of the frame, and the areas that frame drew are copied into the
// other buffer once the switch is proven. No ESP-IDF here (host_tests/display_flip_test.c).

// The buffer LVGL's strips go into: the one the DMA is not reading (two buffers)
unsigned display_back_fb(unsigned front);

// Panel areas drawn since the buffers were last equal. Areas that tile exactly (the 80-pixel strips of one LVGL area,
// a rectangle inside another) merge; when the list is full the new area joins the one it grows least, so the list may
// cover more than was drawn but never less.
#define DISPLAY_DIRTY_MAX 16
typedef struct {
    display_rect_t r[DISPLAY_DIRTY_MAX];
    uint32_t       n;
} display_dirty_t;

void display_dirty_clear(display_dirty_t *d);
void display_dirty_add(display_dirty_t *d, display_rect_t r);

// Copy one rectangle between two frame buffers of `stride_px` pixels per row
void display_copy_rect(uint16_t *dst, uint16_t const *src, int32_t stride_px, display_rect_t r);

// The end-of-frame callback's count and the low 32 bits of a microsecond clock at its last run, or the same pair read
// right after a switch was requested.
typedef struct {
    uint32_t count;
    uint32_t us;
} display_frame_mark_t;

// Whether the frame callbacks seen by `now` prove the DMA restarted on the buffer requested at `req`. The driver reads
// its buffer index a few instructions before it runs the callback, so the first callback after the request may still
// have read the old index unless it ran more than `guard_us` later; the second one cannot have. Differences are taken
// modulo 2^32, the count settles a long idle, the stamp only the first frame.
bool display_swap_proven(display_frame_mark_t req, display_frame_mark_t now, uint32_t guard_us);

// The hardware side, injected so the state machine below runs on the host
typedef struct {
    // Ask the scan-out to switch to buffer `fb` at the next end of frame; fill `mark` right after. False = refused.
    bool (*request)(void *ctx, unsigned fb, display_frame_mark_t *mark);
    // Bounded wait until the switch requested at `mark` is proven. False = not proven in time.
    bool (*wait)(void *ctx, display_frame_mark_t mark);
    // Copy one rectangle from buffer `src` to buffer `dst`, written back so the scan-out sees it
    void (*copy)(void *ctx, unsigned dst, unsigned src, display_rect_t r);
    uint32_t (*now_us)(void *ctx);
    void *ctx;
} display_flip_ops_t;

typedef struct {
    uint32_t swaps;    // proven switches
    uint32_t timeouts; // a wait that ran out: no switch assumed, retried at the next frame end
    uint32_t refusals; // request refused: the frame was copied into the shown buffer instead
    uint32_t wait_max_us, copy_max_us;
    uint64_t wait_us, copy_us;
} display_flip_stats_t;

typedef struct {
    display_flip_ops_t   ops;
    unsigned             front;   // the buffer proven to be scanned out
    bool                 swap_on; // false: one buffer, everything drawn straight into the front
    bool                 pending; // a switch was requested and is not resolved yet
    display_frame_mark_t req;
    display_dirty_t      dirty; // where the back buffer is newer than the front one
    display_flip_stats_t stats;
} display_flip_t;

// The DMA starts on buffer 0 and both start equal
void display_flip_init(display_flip_t *f, display_flip_ops_t const *ops, bool swap_on);

// Before a strip is written: resolves a pending switch (bounded wait; when proven, front flips and the areas the
// previous frames drew are copied into the new back buffer), then returns the buffer to write.
// On a timeout nothing flips and nothing is copied: the strips keep going into the same back buffer, which already
// holds every earlier frame, the areas keep accumulating, and the next frame end requests the same buffer again. A
// stalled scan-out can tear a frame but never leaves a stale picture once it resumes.
unsigned display_flip_write_target(display_flip_t *f);

// A strip landed in the buffer display_flip_write_target returned
void display_flip_drawn(display_flip_t *f, display_rect_t r);

// After the last strip of a refresh: request the switch and return at once (no wait). When the request is refused the
// drawn areas are copied into the shown buffer now, torn but correct. An idle panel after this shows the new buffer
// from the next end of frame; the other buffer is brought level at the next write.
void display_flip_frame_end(display_flip_t *f);
