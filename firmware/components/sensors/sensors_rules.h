#pragma once
// "fw sensors" sub-command rules (#464, console_sub.h): included once by sensors_cmd.c, which registers them, and by the
// host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// The readings (bare, raw) and the boot setting's state stay open; start, stop and "boot on|off" change what runs and
// what boots: developer mode
static console_sub_t const SENSORS_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {"raw", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {"boot", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};
