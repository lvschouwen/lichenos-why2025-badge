#pragma once
// "fw sd" sub-command rules (#464, console_sub.h): included once by sd_console.c, which registers them, and by the host
// pin (host_tests/console_pin_test.c). The words match sd_console.c's own dispatch exactly.

#include "console_sub.h"

static console_sub_t const SD_RULES[] = {
    // #378: test writes 64 KB to the card, cat prints a file; debug commands, not in a release image
    {CON_DEV_WORD("test"), CON_MATCH_EXACT, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    {CON_DEV_WORD("cat"), CON_MATCH_WORD, CON_GATE_DEV, CON_ORIGIN_ANY, 0},
    // #455: wipes the card (journal, settings backup): the USB console only; the screen's format still works
    {"format", CON_MATCH_WORD, CON_GATE_OPEN, CON_ORIGIN_UART, CON_F_ERASE},
    CONSOLE_SUB_END,
};
