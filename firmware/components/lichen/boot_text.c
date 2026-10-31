#include "boot_text.h"

#include <stdio.h>

// "..." not the ellipsis glyph: the badge font lacks it
static char const *const sentences[BOOT_STEP_COUNT] = {
    "Opening storage...", "Starting the screen...", "Checking memory...", "Waking the radio chip...",
    "Connecting to Wi-Fi...", "Setting the clock...", "Starting LoRa...", "Starting apps...",
};

char const *boot_step_sentence(boot_step_t step, int pct, char *buf, size_t cap) {
    if (!buf || cap == 0) {
        return "";
    }
    if (step < 0 || step >= BOOT_STEP_COUNT) {
        buf[0] = '\0';
    } else if (step == BOOT_C6 && pct >= 0) {
        snprintf(buf, cap, "Updating the radio chip (%d %%)...", pct);
    } else {
        snprintf(buf, cap, "%s", sentences[step]);
    }
    return buf;
}

int boot_overall_pct(boot_view_t const *v) {
    int over = 0, total = 0;
    for (int i = 0; i < BOOT_STEP_COUNT; i++) {
        if (i == BOOT_CLOCK) {
            continue;
        }
        total++;
        step_state_t s = v->state[i];
        if (s == STEP_DONE || s == STEP_FAILED || s == STEP_SKIPPED) {
            over++;
        }
    }
    return over * 100 / total;
}
