#pragma once

#include "display_rot.h"

#include <stdbool.h>
#include <stdint.h>

// Pure half of the tear-free flush (#467): two DSI frame buffers, the strips of a frame go into the one that is not
// scanned out, the buffers swap at the end of the frame, and the areas that frame drew are then copied into the other
// buffer so both hold the same picture again. No ESP-IDF here (host_tests/display_flip_test.c).

// The buffer LVGL's strips go into: the one the DMA is not reading (two buffers)
unsigned display_back_fb(unsigned front);

// Panel areas drawn in the current frame. Areas that tile exactly (the 80-pixel strips of one LVGL area, a rectangle
// inside another) merge; when the list is full the new area joins the one it grows least, so the list may cover more
// than was drawn but never less.
#define DISPLAY_DIRTY_MAX 16
typedef struct {
    display_rect_t r[DISPLAY_DIRTY_MAX];
    uint32_t       n;
} display_dirty_t;

void display_dirty_clear(display_dirty_t *d);
void display_dirty_add(display_dirty_t *d, display_rect_t r);

// Copy one rectangle between two frame buffers of `stride_px` pixels per row
void display_copy_rect(uint16_t *dst, uint16_t const *src, int32_t stride_px, display_rect_t r);

// Whether a frame-done callback stamped `cb_us` proves the DMA restarted on the new buffer, given the switch was
// requested before `after_us`. The driver reads its buffer index a few instructions before it calls the callback, so
// a callback stamped within `guard_us` after the request may have read the old index: wait for the next one. Both
// stamps are the low 32 bits of a microsecond clock; the difference is taken modulo 2^32.
bool display_swap_proven(uint32_t after_us, uint32_t cb_us, uint32_t guard_us);
