#pragma once
// "fw psram" sub-command rules (#464, console_sub.h): included once by psram_guard.c, which registers them, and by the
// host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// The bare status stays open; test, dma, dl, dlboot, speed and carrier change timing or the next boot: developer mode
static console_sub_t const PSRAM_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};
