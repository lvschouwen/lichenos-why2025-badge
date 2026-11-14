#pragma once
// "fw lora" sub-command rules (#464, console_sub.h): included once by lora.c, which registers them, and by the host pin
// (host_tests/console_pin_test.c). No form has an origin rule (as before #464).

#include "console_sub.h"

static console_sub_t const LORA_RULES[] = {
    // switches the radio on (#222): on the build check's allow list (tools/check_console_rules.py) until the owner
    // decides; "off" only stops transmitting
    {"on", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_TX},
    CONSOLE_SUB_END,
};
