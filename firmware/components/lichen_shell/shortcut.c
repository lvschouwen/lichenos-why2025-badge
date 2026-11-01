#include "shortcut.h"

#include "tca8418_keymap.h"

#include <stddef.h>

shortcut_t shortcut_of_code(uint8_t code) {
    if (code < 1 || code > sizeof(tca8418_keymap) / sizeof(tca8418_keymap[0])) {
        return SHORTCUT_NONE;
    }
    switch (tca8418_keymap[code - 1]) {
        case KEY_SCANCODE_SQUARE: return SHORTCUT_HOME;
        case KEY_SCANCODE_TRIANGLE: return SHORTCUT_MESSAGES;
        case KEY_SCANCODE_CROSS: return SHORTCUT_SWITCH_APP;
        case KEY_SCANCODE_CIRCLE: return SHORTCUT_SEARCH;
        case KEY_SCANCODE_CLOUD: return SHORTCUT_NOTIFICATIONS;
        case KEY_SCANCODE_DIAMOND: return SHORTCUT_POWER;
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

int shortcut_top_app(int const *ids, int depth, int app_count) {
    for (int i = depth - 1; i >= 0; i--) {
        if (ids[i] >= 1 && ids[i] <= app_count) {
            return ids[i] - 1;
        }
    }
    return -1;
}
