#pragma once
// "fw nvs-down" and "fw nvs-enc" sub-command rules (#464, console_sub.h): included once by lichen_nvs.c, which
// registers them, and by the host pin (host_tests/console_pin_test.c)

#include "console_sub.h"

// The bench's "next boot without NVS" (LICHEN_NVS_DEBUG_FORCE_DOWN): restarts at once; the USB console only, every form
static console_sub_t const NVS_DOWN_RULES[] = {
    {NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_RESTART},
    CONSOLE_SUB_END,
};

// The IRREVERSIBLE eFuse burn (LICHEN_NVS_ENCRYPT_BURN_CMD): the USB console only. Without the arm the row has no word.
#if CONFIG_LICHEN_NVS_ENCRYPT_BURN_CMD
#define NVS_BURN_WORD "burn YES"
#else
#define NVS_BURN_WORD NULL
#endif
static console_sub_t const NVS_ENC_RULES[] = {
    {NVS_BURN_WORD, CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, 0},
    CONSOLE_SUB_END,
};
