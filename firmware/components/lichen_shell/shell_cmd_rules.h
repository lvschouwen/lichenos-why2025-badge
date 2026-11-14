#pragma once
// lichen_shell's sub-command rules (#464, console_sub.h): "fw stress" (app_stress.c), "fw idle" (screen_idle.c) and
// "fw setup" (setup_screens.c). Included once by each, and by the host pin (host_tests/console_pin_test.c). Each lists
// its open forms exactly; every other form needs developer mode (a release answers "not in this build").

#include "console_sub.h"

// A bare "stress" starts a run; status and stop stay open, so a run can always be ended
static console_sub_t const STRESS_RULES[] = {
    {"status", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {"stop", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};

// "idle test <s>" fakes the idle clock
static console_sub_t const IDLE_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};

// reset and done write the first-run flags: no open form
static console_sub_t const SETUP_RULES[] = {
    {NULL, CON_MATCH_REST, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};
