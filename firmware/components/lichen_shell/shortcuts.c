// The shape keys as fixed OS shortcuts (#79): ■ home, ▲ messages, ✕ switch app, ● search, ☁ notifications, ◆ power.
// The keyboard maps them to no LVGL key (kbd_map.c), so no app or text field ever sees them; the shell hears them as raw
// key events (raw_keys.c, LVGL context) before any focus and acts here. shortcut.c decides, this file does.
#include "esp_log.h"
#include "lichen_app.h"
#include "nav.h"
#include "raw_keys.h"
#include "shell_parts.h"
#include "shortcut.h"
#include "update_screen.h"

#include <string.h>

static char const TAG[] = "shortcuts";

static app_switch_t switcher = APP_SWITCH_INIT; // LVGL task only

void shortcuts_app_opened(size_t index) {
    app_switch_opened(&switcher, (int)index);
}

static int app_showing(void) {
    int ids[NAV_MAX_DEPTH];
    int depth = nav_depth();
    for (int i = 0; i < depth; i++) {
        ids[i] = nav_at(i);
    }
    return shortcut_top_app(ids, depth, (int)lichen_app_count());
}

static int app_index_of(char const *id) {
    for (size_t i = 0; i < lichen_app_count(); i++) {
        if (strcmp(lichen_app_at(i)->id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// From home, so Esc leads back there and the stack never fills up with shortcut hops
static bool open_from_home(int index) {
    if (index < 0) {
        return false;
    }
    shell_home();
    shell_open_app((size_t)index);
    return nav_current() == 1 + index; // shell_open refuses while booting or with the stack full
}

static void open_messages(void) {
    for (size_t i = 0; i < lichen_app_count(); i++) {
        lichen_app_t const *app = lichen_app_at(i);
        if (app->open_messages) {
            if (open_from_home((int)i)) {
                app->open_messages();
            }
            return;
        }
    }
    ESP_LOGW(TAG, "no app takes the Messages key");
}

// Power and the notifications open over the current screen: Esc goes back to where the key was pressed
static void open_over(char const *id) {
    int i = app_index_of(id);
    if (i >= 0 && nav_current() != 1 + i) {
        shell_open_app((size_t)i);
    }
}

void shortcuts_run(shortcut_t s) {
    ESP_LOGW(TAG, "%s", shortcut_name(s));
    switch (s) {
        case SHORTCUT_HOME: shell_home(); break;
        case SHORTCUT_MESSAGES: open_messages(); break;
        case SHORTCUT_SWITCH_APP: open_from_home(app_switch_target(&switcher, app_showing())); break;
        case SHORTCUT_SEARCH: shell_search(); break;
        case SHORTCUT_NOTIFICATIONS: notify_ui_cloud_key(); break;
        case SHORTCUT_POWER: open_over("power"); break;
        default: break;
    }
}

void shortcuts_key(uint8_t code, bool pressed, bool test_open) {
    shortcut_gate_t gate = {
        .dialog_open   = shell_dialog_group() != NULL,
        .test_open     = test_open,
        .booting       = !shell_home_shown(),
        .update_busy   = update_screen_busy() || power_going_down(),
        .setup_running = setup_running(),
    };
    shortcut_t s = shortcut_decide(code, pressed, &gate);
    if (s != SHORTCUT_NONE) {
        shortcuts_run(s);
    } else if (pressed && shortcut_of_code(code) != SHORTCUT_NONE && !test_open) {
        ESP_LOGW(TAG, "%s: not now", shortcut_name(shortcut_of_code(code)));
    }
}

static void on_raw_key(key_raw_event_t const *ev, void *arg) {
    (void)arg;
    shortcuts_key(ev->code, ev->pressed, ev->test_open);
}

void shortcuts_start(void) {
    if (!raw_keys_listen(on_raw_key, NULL, false)) {
        ESP_LOGE(TAG, "shape keys not wired: no raw key listener free");
    }
}
