#include "shortcut.h"

#include "key_matrix.h"
#include "screen_ids.h"

#include <stddef.h>

shortcut_t shortcut_of_code(uint8_t code) {
    switch (key_matrix_shape(code)) {
        case KEY_SHAPE_SQUARE: return SHORTCUT_HOME;
        case KEY_SHAPE_TRIANGLE: return SHORTCUT_MESSAGES;
        case KEY_SHAPE_CROSS: return SHORTCUT_SWITCH_APP;
        case KEY_SHAPE_CIRCLE: return SHORTCUT_SEARCH;
        case KEY_SHAPE_CLOUD: return SHORTCUT_NOTIFICATIONS;
        case KEY_SHAPE_DIAMOND: return SHORTCUT_POWER;
        default: return SHORTCUT_NONE;
    }
}

shortcut_t shortcut_decide(uint8_t code, bool pressed, shortcut_gate_t const *gate) {
    if (!pressed || gate->dialog_open || gate->test_open || gate->booting || gate->update_busy ||
        gate->setup_running) {
        return SHORTCUT_NONE;
    }
    return shortcut_of_code(code);
}

char const *shortcut_name(shortcut_t s) {
    switch (s) {
        case SHORTCUT_HOME: return "home";
        case SHORTCUT_MESSAGES: return "messages";
        case SHORTCUT_SWITCH_APP: return "switch";
        case SHORTCUT_SEARCH: return "search";
        case SHORTCUT_NOTIFICATIONS: return "notifications";
        case SHORTCUT_POWER: return "power";
        default: return "none";
    }
}

void app_switch_opened(app_switch_t *s, int index) {
    if (index < 0 || index == s->cur) {
        return; // the same app again (Settings > Power from Power ...) changes nothing
    }
    s->prev = s->cur;
    s->cur  = index;
}

int app_switch_target(app_switch_t const *s, int showing) {
    if (showing >= 0 && showing == s->cur) {
        return s->prev;
    }
    return s->cur;
}

int shortcut_top_app(int const *ids, int depth) {
    for (int i = depth - 1; i >= 0; i--) {
        if (ids[i] > SCREEN_APP_OWN && ids[i] <= SCREEN_APP_OWN + SCREEN_APP_MAX) {
            return ids[i] - SCREEN_APP_OWN;
        }
    }
    return -1;
}
