#include "boot_steps.h"

#include <string.h>

static boot_view_t view = {.pct = -1};

static char const *const labels[BOOT_STEP_COUNT] = {"storage", "display", "PSRAM", "C6", "Wi-Fi", "clock", "LoRa", "apps"};

static int valid(boot_step_t step) {
    return step >= 0 && step < BOOT_STEP_COUNT;
}

void boot_steps_reset(void) {
    uint32_t generation = view.generation + 1;
    memset(&view, 0, sizeof(view));
    view.pct        = -1;
    view.generation = generation;
}

void boot_step_begin(boot_step_t step) {
    view.generation++;
    if (!valid(step)) {
        return;
    }
    view.state[step]     = STEP_ACTIVE;
    view.reason[step][0] = '\0';
    view.current         = step;
    view.pct             = -1;
}

void boot_step_progress(boot_step_t step, int pct) {
    view.generation++;
    if (!valid(step) || view.state[step] != STEP_ACTIVE) {
        return;
    }
    view.current = step;
    view.pct     = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void boot_step_end(boot_step_t step, step_state_t state, char const *reason) {
    view.generation++;
    if (!valid(step)) {
        return;
    }
    view.state[step] = state;
    size_t len       = reason ? strlen(reason) : 0;
    if (len > BOOT_REASON_LEN - 1) {
        len = BOOT_REASON_LEN - 1;
        // Don't cut a UTF-8 sequence in half: back up over continuation bytes (10xxxxxx)
        while (len > 0 && ((unsigned char)reason[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    if (len) {
        memcpy(view.reason[step], reason, len);
    }
    view.reason[step][len] = '\0';
    if (step == view.current) {
        view.pct = -1;
    }
}

void boot_steps_view(boot_view_t *out) {
    *out = view;
}

char const *boot_step_label(boot_step_t step) {
    return valid(step) ? labels[step] : "?";
}

bool boot_step_failable(boot_step_t step) {
    return valid(step) && step != BOOT_PSRAM;
}

int boot_fail_step(uint8_t flag) {
    if (flag == 0 || flag > BOOT_STEP_COUNT || !boot_step_failable((boot_step_t)(flag - 1))) {
        return -1;
    }
    return flag - 1;
}
