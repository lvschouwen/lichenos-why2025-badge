#pragma once
// "fw crash" sub-command rules (#464, console_sub.h): included once by crash.c, which registers them, and by the host
// pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// The test arms crash the badge on purpose (a panic, the task watchdog): developer mode, not in a release image. Open to
// every origin, as before #464: on the build check's allow list for the owner to decide.
static console_sub_t const CRASH_RULES[] = {
    // erases the crash log and the core dump partition; open from every origin, as before #464
    {"clear", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_ERASE},
    {CON_DEV_WORD("test panic"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART},
    {CON_DEV_WORD("test wdt"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, CON_F_RESTART},
    CONSOLE_SUB_END,
};
