#pragma once
// The sub-command rules of fw_console's own commands (#464, console_sub.h): included once by console.c, which registers
// them, and by the host pin (host_tests/console_pin_test.c), so the rows the badge checks are the rows the pin tests.

#include "console_sub.h"

// "slot other" boots the other image, which may not keep what the USB hold guards (#420)
static console_sub_t const OS_SLOT_RULES[] = {
    {"other", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_USB_HOLD | CON_F_RESTART},
    CONSOLE_SUB_END,
};

// "fw reboot" over Wi-Fi is how tools/badge.sh restarts the badge (on the build check's allow list)
static console_sub_t const OS_REBOOT_RULES[] = {
    {NULL, CON_MATCH_REST, CON_GATE_OPEN, CON_ORIGIN_ANY, CON_F_RESTART},
    CONSOLE_SUB_END,
};

// "dev on" from the USB console only (#373): the Wi-Fi console's OTA token is not enough, and keys injected into the
// Console app could type it there; About's taps are the owner's other way in. Off works from anywhere.
static console_sub_t const OS_DEV_RULES[] = {
    {"on", CON_MATCH_EXACT, CON_GATE_OPEN, CON_ORIGIN_UART, 0},
    CONSOLE_SUB_END,
};
