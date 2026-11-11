#pragma once
// The bench test card's layout (#352), pure and host-tested in host_tests/test_card_layout_test.c: where every element
// sits on the 720x720 panel, the wedge levels, the bar colours and order, the marker's place per frame, the console
// argument and the info strip's text. test_card.c draws it with LVGL; the console overlay (`fw ui card`) and the Test
// card app share both.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TCARD_SIZE       720
#define TCARD_GRID_STEP  60
#define TCARD_GRID_LINES 11 // inner lines per direction: 60, 120 ... 660
#define TCARD_BARS       8
#define TCARD_WEDGE      11 // 0, 10 ... 100 %
#define TCARD_GRATINGS   4
#define TCARD_MARKER_POS 24 // positions along the bottom edge; the marker moves one per frame
#define TCARD_CORNER_RECTS 8 // four corners, two arms each

typedef struct {
    int16_t x, y, w, h;
} tcard_rect_t;

typedef enum { TCARD_TEST, TCARD_WHITE, TCARD_GREY, TCARD_BLACK, TCARD_OFF, TCARD_MODES } tcard_mode_t;

// "fw ui card <args>": "" and "test" give TCARD_TEST; false for anything unknown. Case-insensitive, spaces around ignored.
bool tcard_mode_parse(char const *args, tcard_mode_t *mode);
// The app's Left/Right cycle over test, white, grey, black (wraps); delta is +1 or -1
tcard_mode_t tcard_mode_step(tcard_mode_t mode, int delta);
// The solid field of white, grey and black as 0xRRGGBB; 0 for the others
uint32_t tcard_solid_color(tcard_mode_t mode);

tcard_rect_t tcard_info_rect(void);   // the text strip at the top
tcard_rect_t tcard_circle_rect(void); // the centre circle (and the white field inside it)
tcard_rect_t tcard_border_rect(void); // the 1 px frame on the outermost pixels
tcard_rect_t tcard_corner_rect(int i); // 0..TCARD_CORNER_RECTS-1: corner marks (an L of two arms each)
tcard_rect_t tcard_grid_rect(int i, bool vertical); // 0..TCARD_GRID_LINES-1, 1 px lines
tcard_rect_t tcard_bar_rect(int i);   // 0..TCARD_BARS-1
uint32_t     tcard_bar_color(int i);  // R, G, B, C, M, Y, white, black as 0xRRGGBB
tcard_rect_t tcard_wedge_rect(int i); // 0..TCARD_WEDGE-1
int          tcard_wedge_percent(int i); // 0, 10 ... 100
uint32_t     tcard_wedge_color(int i);   // the step as a grey 0xRRGGBB
tcard_rect_t tcard_marker_track(void);
tcard_rect_t tcard_marker_rect(uint32_t frame); // one position per frame, wraps after TCARD_MARKER_POS

typedef struct {
    tcard_rect_t area;
    bool         vertical; // vertical lines (stripes side by side) or horizontal ones
    int          line_px;  // 1 or 2: a line and an equal gap
} tcard_grating_t;
tcard_grating_t tcard_grating(int i);
// The i-th line of a grating (0-based) inside its area; false past the last one that fits
bool tcard_grating_line(tcard_grating_t const *g, int i, tcard_rect_t *out);

// The info strip's two lines. in->fw: "fw-<sha>" (NULL reads "fw ?"). in->refresh_raw / bl_*_raw are lichen_reg_get's text
// ("468 (3.0 s ago)", "? (not read yet)", NULL or "" when absent); refresh is in dHz. in->req_disp < 0: unknown, the line then shows applied only; in->req_*: the levels asked for.
typedef struct {
    char const *fw, *refresh_raw, *bl_disp_raw, *bl_kbd_raw;
    int         req_disp, req_kbd;
} tcard_info_t;
void tcard_info_text(tcard_info_t const *in, char *line1, size_t cap1, char *line2, size_t cap2);
