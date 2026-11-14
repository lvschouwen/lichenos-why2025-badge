#pragma once
// "fw c6" sub-command rules (#464, console_sub.h): included once by c6link.c, which registers them, and by the host pin
// (host_tests/console_pin_test.c). No form has an origin rule (as before #464): every restarting row is on the build
// check's allow list (tools/check_console_rules.py) until the owner decides each.

#include "console_sub.h"

static console_sub_t const C6_RULES[] = {
    {"reset", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART},     // the C6 (#14)
    {"reset rom", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART}, // the C6, into its ROM loader
    {"reattach", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART},  // resets the C6, Wi-Fi drops
    {"verify", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART},    // restarts the badge
    {"reflash", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART},   // the next boot flashes the C6
    CONSOLE_SUB_END,
};
