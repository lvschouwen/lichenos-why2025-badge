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
