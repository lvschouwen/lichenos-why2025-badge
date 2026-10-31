// The backlight command the P4 sends over the C6 console UART: "BL <display%> <keyboard%>\n"
#pragma once

#include <stdbool.h>

// Parse one line (without the newline). Percentages must be 0..100; the PWM code applies its own 10-80% clamp.
bool backlight_cmd_parse(char const *line, int *display, int *keyboard);
