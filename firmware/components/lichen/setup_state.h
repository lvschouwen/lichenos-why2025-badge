#pragma once

#include <stdbool.h>

// First-run setup steps (#66). Pure, host-tested in host_tests/setup_state_test.c; setup_screens.c shows them.
typedef enum { SETUP_WELCOME, SETUP_WIFI, SETUP_DONE } setup_step_t;

// DONE when setup_done is saved, else the welcome
setup_step_t setup_first(bool setup_done);
// Welcome goes to Wi-Fi, or straight to DONE when the badge already has credentials. Wi-Fi goes to DONE whether the
// person joined or skipped.
setup_step_t setup_next(setup_step_t step, bool have_wifi_credentials);
// Esc: Wi-Fi back to the welcome (nothing is saved); the welcome stays
setup_step_t setup_back(setup_step_t step);
