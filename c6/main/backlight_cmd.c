// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
#include "backlight_cmd.h"

#include <stdio.h>

bool backlight_cmd_parse(char const *line, int *display, int *keyboard) {
    int  d, k;
    char extra;
    // %c after the numbers catches trailing junk: a well-formed line has nothing after the keyboard value
    if (sscanf(line, "BL %d %d %c", &d, &k, &extra) != 2) {
        return false;
    }
    if (d < 0 || d > 100 || k < 0 || k > 100) {
        return false;
    }
    *display  = d;
    *keyboard = k;
    return true;
}

uint32_t backlight_duty(int percent, unsigned resolution_bits) {
    if (percent <= 0) {
        return 0;
    }
    int const held = percent < 10 ? 10 : percent > 80 ? 80 : percent;
    return ((uint32_t)1 << resolution_bits) * (uint32_t)held / 100u;
}
