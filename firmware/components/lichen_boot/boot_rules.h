#pragma once
// "fw boot" and "fw factory-reset" sub-command rules (#464, console_sub.h): included once each by boot_progress.c and
// factory_reset.c, which register them, and by the host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

static console_sub_t const BOOT_RULES[] = {
    // #379: arms a key combo for the next software restart; a release answers "not in this build"
    {"keys", CON_MATCH_WORD, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    // marks a start-up step failed on the next boot; the arm is not in a release image
    {CON_DEV_WORD("fail "), CON_MATCH_PREFIX, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};

// #455, epic-395 rule 4: the reset erases storage and restarts, so the USB console only, held or not. Fail closed: the
// dry run (plan) and the usage stay open, every other form (sscanf in cmd_factory takes any spacing) takes the rule.
static console_sub_t const FACTORY_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {"plan", CON_MATCH_WORD, CON_GATE_OPEN, CON_ORIGIN_ANY, 0},
    {NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_ERASE | CON_F_RESTART},
    CONSOLE_SUB_END,
};
