#include "attention_policy.h"

#include "lichen_sys_display.h"

#include <string.h>

attn_mode_t attn_mode_from_u8(uint8_t raw) {
    return raw < ATTN_MODE_COUNT ? (attn_mode_t)raw : ATTN_MODE_DEFAULT;
}

attn_mode_t attn_mode_next(attn_mode_t mode) {
    return (attn_mode_t)((mode + 1) % ATTN_MODE_COUNT);
}

char const *attn_mode_name(attn_mode_t mode) {
    return mode == ATTN_OFF ? "Off" : mode == ATTN_DMS ? "Direct messages" : "All";
}

bool attn_is_dm(char const *target) {
    return target && strncmp(target, "dm:", 3) == 0;
}

bool attn_wants_pulse(attn_mode_t mode, char const *target, uint8_t notice_level, attn_state_t state) {
    if (mode == ATTN_OFF || notice_level >= 2 || state.dnd || !state.screen_lit || state.hot || state.flashlight) {
        return false; // an error has its own red toast; a pulse is for news
    }
    return mode == ATTN_ALL || attn_is_dm(target);
}

int attn_keyboard_at(uint32_t elapsed_ms) {
    if (elapsed_ms >= ATTN_PULSE_MS) {
        return -1;
    }
    return (elapsed_ms / 150) % 2 == 0 ? ATTN_PEAK : 0;
}

attn_levels_t attn_flash_levels(bool hot) {
    attn_levels_t l = hot ? (attn_levels_t){LICHEN_SYS_DISPLAY_MIN, 0} : (attn_levels_t){LICHEN_SYS_DISPLAY_MAX, LICHEN_SYS_DISPLAY_MAX};
    return l;
}
