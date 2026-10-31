#include "setup_state.h"

setup_step_t setup_first(bool setup_done) {
    return setup_done ? SETUP_DONE : SETUP_WELCOME;
}

setup_step_t setup_next(setup_step_t step, bool have_wifi_credentials) {
    if (step == SETUP_WELCOME) {
        return have_wifi_credentials ? SETUP_DONE : SETUP_WIFI;
    }
    return SETUP_DONE;
}

setup_step_t setup_back(setup_step_t step) {
    return step == SETUP_WIFI ? SETUP_WELCOME : step;
}
