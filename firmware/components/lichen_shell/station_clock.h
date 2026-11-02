#pragma once

#include <stdint.h>

// The Swiss railway style station clock screensaver (#119), pure and host-tested in host_tests/station_clock_test.c:
// the hands' angles from the time (with the stop-to-go second hand), the dial's proportions, the end points of a bar
// and the boxes to redraw when a hand moves. saver_station.c draws it.
//
// Angles are in tenths of a degree, clockwise from 12 (0-3599). Coordinates are pixels, y down.

// Stop-to-go: the second hand sweeps the dial in SC_SWEEP_MS, waits at 12 for the rest of the minute, and the minute
// hand jumps on the minute (when the real clock's seconds reach :00), as the sweep starts again
#define SC_SWEEP_MS  58500u
#define SC_MINUTE_MS 60000u

// ms: milliseconds into the minute (sec * 1000 + ms, 0-59999; more counts as the pause). 0 during the pause.
int32_t sc_second_angle(uint32_t ms);
// The minute hand stands on whole minutes; the hour hand moves a little every minute (half a degree)
int32_t sc_minute_angle(int min);
int32_t sc_hour_angle(int hour, int min);

typedef struct {
    int32_t x, y;
} sc_point_t;

// Inclusive corners, as an lv_area_t
typedef struct {
    int32_t x1, y1, x2, y2;
} sc_box_t;

// A straight bar along a radius: from r0 to r1 from the centre (negative: the tail behind the centre), w wide
typedef struct {
    int32_t r0, r1, w;
} sc_bar_t;

// The dial's parts for a dial of radius r, all inside it
typedef struct {
    sc_bar_t hour_mark, minute_mark; // the dial's 12 thick and 48 thin bars
    sc_bar_t hour, minute;           // broad and long slim black hands
    sc_bar_t second;                 // the thin red bar, from its tail to the centre of its disc
    int32_t  disc_r;                 // the red disc at the end of the second hand
    int32_t  cap_r;                  // the red cap on the centre
} sc_dims_t;
sc_dims_t sc_dims(int32_t r);

// The point at distance r from (cx, cy) at angle a (r < 0: the opposite way)
sc_point_t sc_polar(int32_t cx, int32_t cy, int32_t a, int32_t r);

// A bar's two end points (the middle of its ends)
void sc_bar_ends(int32_t cx, int32_t cy, int32_t a, sc_bar_t bar, sc_point_t *p0, sc_point_t *p1);

// The box around the part of a bar from r0 to r1 (pass the bar's own r0, r1 for all of it), grown by pad on every side
// for the anti-aliased edge. Splitting a long diagonal hand in a few pieces keeps the redrawn area small.
sc_box_t sc_bar_box(int32_t cx, int32_t cy, int32_t a, int32_t w, int32_t r0, int32_t r1, int32_t pad);

// The box around a disc of radius dr whose centre is r from (cx, cy) at angle a, grown by pad
sc_box_t sc_disc_box(int32_t cx, int32_t cy, int32_t a, int32_t r, int32_t dr, int32_t pad);
