#pragma once
// main's sub-command rules (#464, console_sub.h): "fw noscreen". Included once by main.c, which registers it, and by the
// host pin (host_tests/console_pin_test.c).

#include "console_sub.h"

// The next boot skips the display start (#281): a test command, every form needs developer mode
static console_sub_t const NOSCREEN_RULES[] = {
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};
