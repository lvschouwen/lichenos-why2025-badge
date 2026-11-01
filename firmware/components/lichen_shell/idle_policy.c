#include "idle_policy.h"

#include <stdio.h>

static uint8_t const DIM_CHOICES[] = {0, 3, 6, 12, 30};   // off, 30 s, 1, 2, 5 min
static uint8_t const OFF_CHOICES[] = {0, 12, 30, 60, 180}; // off, 2, 5, 10, 30 min

idle_level_t idle_level(uint32_t idle_s, idle_times_t t, bool inhibit) {
    if (inhibit) {
        return IDLE_BRIGHT;
    }
    if (t.off_s && idle_s >= t.off_s) {
        return IDLE_OFF;
    }
    if (t.dim_s && idle_s >= t.dim_s) {
        return IDLE_DIM;
    }
    return IDLE_BRIGHT;
}

bool idle_key_swallowed(idle_level_t shown) {
    return shown == IDLE_OFF;
}

idle_times_t idle_times_from_10s(uint8_t dim_10s, uint8_t off_10s) {
    return (idle_times_t){.dim_s = dim_10s * 10u, .off_s = off_10s * 10u};
}

static uint8_t next_choice(uint8_t const *choices, size_t n, uint8_t current) {
    for (size_t i = 0; i < n; i++) {
        if (choices[i] == current) {
            return choices[(i + 1) % n];
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (choices[i] > current) {
            return choices[i];
        }
    }
    return choices[0];
}

uint8_t idle_dim_next(uint8_t current_10s) {
    return next_choice(DIM_CHOICES, sizeof(DIM_CHOICES), current_10s);
}

uint8_t idle_off_next(uint8_t current_10s) {
    return next_choice(OFF_CHOICES, sizeof(OFF_CHOICES), current_10s);
}

void idle_choice_text(uint8_t value_10s, char *out, size_t cap) {
    unsigned s = value_10s * 10u;
    if (!s) {
        snprintf(out, cap, "off");
    } else if (s < 60 || s % 60) {
        snprintf(out, cap, "%u s", s);
    } else {
        snprintf(out, cap, "%u min", s / 60);
    }
}

char const *idle_level_name(idle_level_t level) {
    switch (level) {
        case IDLE_BRIGHT: return "bright";
        case IDLE_DIM: return "dim";
        case IDLE_OFF: return "off";
    }
    return "?";
}
