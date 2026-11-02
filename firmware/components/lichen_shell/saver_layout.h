#pragma once

#include <stdint.h>

// The clock screensaver's drawing (#75), pure and host-tested in host_tests/saver_layout_test.c: seven-segment digits
// built from rectangles (the UI fonts stop at 40 px), and the drift that moves the clock a little every minute so
// nothing stays lit on the same pixels.

// Segments a-g as bits 0-6 (a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle)
#define SEG7_COUNT 7
// The segments lit for '0'-'9' and '-' (an unset clock); 0 for anything else
uint8_t seg7_mask(char c);

typedef struct {
    int32_t x, y, w, h;
} seg_rect_t;
// Where segment seg (0-6) sits in a digit box w x h with bars t thick
seg_rect_t seg7_rect(int seg, int32_t w, int32_t h, int32_t t);

// The clock's offset from the centre at drift step `step` (minutes): within [-span_x, span_x] x [-span_y, span_y],
// (0, 0) at step 0, a different place every step, bouncing between the edges so it sweeps the whole area
void saver_drift(uint32_t step, int32_t span_x, int32_t span_y, int32_t *x, int32_t *y);
