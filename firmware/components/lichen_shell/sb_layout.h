#pragma once

#include <stdint.h>

// The status bar's right group against the centred clock (#399), pure: widths in, what to show out.

#define SB_NOTIFY_DOT_PX 10
#define SB_NOTIFY_GAP   4 // between the dot and the count
#define SB_NOTIFY_PAD_R 8 // the chip's right padding
#define SB_NOTIFY_WIFI  6 // the Wi-Fi row's gap after the chip
// What the chip adds to the right group with the dot alone
#define SB_NOTIFY_DOT_W (SB_NOTIFY_DOT_PX + SB_NOTIFY_PAD_R + SB_NOTIFY_WIFI)

typedef enum {
    SB_NOTIFY_COUNT, // dot and count
    SB_NOTIFY_DOT,   // the dot alone: the count is the lowest-priority part and gives way first
    SB_NOTIFY_NONE,  // not even the dot fits: hidden rather than over the clock
} sb_notify_t;

// Pixels free for the notification chip between the clock (kept CLEAR px away from its right edge) and the right
// group (right_w wide without the chip, BAR_PAD from the screen edge). The clock is centred and as wide as its widest
// text, so it never moves; the chip yields.
int32_t sb_notify_room(int32_t screen_w, int32_t bar_pad, int32_t clock_w, int32_t clear, int32_t right_w);

// The widest form that fits in room; dot_w and count_w are the chip's extra width with the dot alone and with the dot
// and count (both include its gaps).
sb_notify_t sb_notify_fit(int32_t room, int32_t dot_w, int32_t count_w);
