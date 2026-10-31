// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// The backlight command the P4 sends over the C6 console UART: "BL <display%> <keyboard%>\n"
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Parse one line (without the newline). Percentages must be 0..100; backlight_duty applies the 10-80% clamp.
bool backlight_cmd_parse(char const *line, int *display, int *keyboard);

// PWM duty for a brightness in percent at the given timer resolution. 0 is off; anything else is held to 10-80%:
// below 10% the screen's boost driver flickers, above 80% only draws more current for little extra light.
uint32_t backlight_duty(int percent, unsigned resolution_bits);
