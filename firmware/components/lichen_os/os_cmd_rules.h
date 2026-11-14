#pragma once
// lichen_os's sub-command rules (#464, console_sub.h): "fw thermal" (thermal.c) and the "fw app" lines safe mode adds
// (safe_mode.c, through lichen_sys_app_console_more). Included once by each, and by the host pin
// (host_tests/console_pin_test.c).

#include "console_sub.h"

// The bare status stays open; every other form (test warn|clear|shutdown YES, seen reset) needs developer mode: test
// can power the badge off, seen reset erases a flag. A release answers "not in this build".
static console_sub_t const THERMAL_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    // "test shutdown YES" powers the badge off (counted with the restarts); the same gate as every other form
    {"test", CON_MATCH_WORD, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART},
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};

// #427: "app crash <id> [cb]" panics as that app, so the badge restarts; developer mode, not in a release image. Open to
// every origin, as before #464: on the build check's allow list for the owner to decide.
static console_sub_t const APP_RULES[] = {
    {CON_DEV_WORD("crash "), CON_MATCH_PREFIX, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART},
    CONSOLE_SUB_END,
};
