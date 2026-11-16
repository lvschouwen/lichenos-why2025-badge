#include "sb_layout.h"

int32_t sb_notify_room(int32_t screen_w, int32_t bar_pad, int32_t clock_w, int32_t clear, int32_t right_w) {
    int32_t clock_right = screen_w / 2 + clock_w / 2;
    int32_t group_left  = screen_w - bar_pad - right_w;
    return group_left - clock_right - clear;
}

sb_notify_t sb_notify_fit(int32_t room, int32_t dot_w, int32_t count_w) {
    if (count_w <= room) {
        return SB_NOTIFY_COUNT;
    }
    return dot_w <= room ? SB_NOTIFY_DOT : SB_NOTIFY_NONE;
}
