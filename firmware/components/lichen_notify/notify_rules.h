#pragma once
// "fw notify" sub-command rules (#464, console_sub.h): included once by notify_cmd.c, which registers them, and by the
// host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// "notify test ..." posts a test notification: developer mode; the other forms are settings a person uses
static console_sub_t const NOTIFY_RULES[] = {
    {"test", CON_MATCH_WORD, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    CONSOLE_SUB_END,
};
