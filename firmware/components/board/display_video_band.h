#pragma once

#include "display_rot.h"

#include <stdbool.h>
#include <stdint.h>

// The held band strips of the OS video surface (#561, part of #527); host_tests/display_video_band_test.c. While an app owns
// the video, LVGL's strips outside the video rectangle (the status bar) are copied into one small PSRAM buffer and LVGL is
// released at once; they ride the next present. This is the bookkeeping only: where each copy sits in the buffer, replace on
// the same rectangle, and the overflow rule. No ESP-IDF, no LVGL, no locking: the LVGL task runs it.

#define DISPLAY_BAND_MAX   16
#define DISPLAY_BAND_ALIGN 64 // a copy starts on a cache line: the PPA's input write-back never shares one

typedef struct {
    display_rect_t ui;  // where the copy goes, UI coordinates
    uint32_t       off; // its byte offset in the band buffer; rows are w * 2 bytes apart (no padding)
} display_band_t;

typedef struct {
    display_band_t e[DISPLAY_BAND_MAX]; // oldest first: applied in this order, so a newer strip wins an overlap
    uint32_t       n, used, cap;        // entries, bytes used, bytes the buffer has
    bool           lost;                // a strip did not fit and was not kept
    display_rect_t lost_rect;           // bounding box of the strips that were not kept (UI coordinates)
    uint32_t       overflows;
} display_band_q_t;

void display_band_q_init(display_band_q_t *q, uint32_t cap_bytes);

// Bytes one rectangle takes in the buffer (rows w * 2 apart, rounded up to DISPLAY_BAND_ALIGN)
uint32_t display_band_bytes(display_rect_t r);

// Room for a strip of rectangle r. The same rectangle again takes its old place (the old pixels are stale) and moves to the
// end of the queue, so order against overlapping strips stays right. NULL when it does not fit (the queue is full or the
// buffer is): nothing is kept, `lost` and `lost_rect` say where, and LVGL is never made to wait for it.
display_band_t const *display_band_q_put(display_band_q_t *q, display_rect_t r);

uint32_t display_band_q_count(display_band_q_t const *q);

// After a present drew the strips: the queue is empty again. `lost` stays until display_band_q_take_lost.
void display_band_q_pop_all(display_band_q_t *q);

// The area to invalidate so LVGL renders the strips that did not fit again, once the queue has room (false: none lost)
bool display_band_q_take_lost(display_band_q_t *q, display_rect_t *r);

// Drop everything (the screen is handed back and fully invalidated, so nothing queued is wanted any more)
void display_band_q_clear(display_band_q_t *q);
