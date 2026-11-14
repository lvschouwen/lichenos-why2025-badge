#pragma once
// "fw token" sub-command rules (#464, console_sub.h): included once by ota_server.c, which registers them, and by the
// host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// #456: a new token guards firmware installs, so the USB console only (the Wi-Fi console and the Console app count as
// remote); it is printed as a secret line, as is the bare "token"
static console_sub_t const TOKEN_RULES[] = {
    {"", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_SECRET},
    {"new", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_SECRET},
    CONSOLE_SUB_END,
};
