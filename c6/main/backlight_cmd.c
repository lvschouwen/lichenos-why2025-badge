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

int backlight_percent(uint32_t duty, unsigned resolution_bits) {
    uint64_t const full = (uint64_t)1 << resolution_bits;
    if (duty >= full) {
        return 100;
    }
    // backlight_duty rounds down, so the duty sits at most one step below percent * full / 100: rounding to nearest
    // gets the percentage back as long as a step is under half a percent (8 bits and up)
    return (int)(((uint64_t)duty * 100u + full / 2) / full);
}
