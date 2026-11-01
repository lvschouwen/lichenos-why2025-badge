#pragma once

#include <stdbool.h>
#include <stdint.h>

// The six shape keys between Esc and Backspace (#79) are fixed OS shortcuts, the same on every screen. This is the
// decision part: which key does what, and when a key does nothing. Pure (no ESP, no LVGL), host-tested in
// host_tests/shortcut_test.c; shortcuts.c carries the actions out on the LVGL task.
typedef enum {
    SHORTCUT_NONE = 0,
    SHORTCUT_HOME,          // ■ home
    SHORTCUT_MESSAGES,      // ▲ the messaging app's message list (MeshCore's Chat)
    SHORTCUT_SWITCH_APP,    // ✕ back to the previous app
    SHORTCUT_SEARCH,        // ● search in the current list or chat
    SHORTCUT_NOTIFICATIONS, // ☁ the showing toast, else the notification centre
    SHORTCUT_POWER,         // ◆ the Power app
} shortcut_t;

// What is going on when the key comes. Any of these set: the key does nothing.
typedef struct {
    bool dialog_open;   // a confirm or countdown dialog has the keys
    bool test_open;     // the Keyboard Test owns every key (it shows the shape keys)
    bool booting;       // home is not up yet (or still fading in)
    bool update_busy;   // a firmware update or the power-down screen is on
    bool setup_running; // first-run setup
} shortcut_gate_t;

// The shape key a TCA8418 code (1-0x50, no press bit) is, SHORTCUT_NONE for every other key
shortcut_t shortcut_of_code(uint8_t code);
// The action for a raw key event: only presses act (the release is left free for a long press later, e.g. #66's boot
// rescue combo); SHORTCUT_NONE when the key is no shortcut or the gate says no
shortcut_t shortcut_decide(uint8_t code, bool pressed, shortcut_gate_t const *gate);
// "home", "messages", ... (fw keys send names, logs); "none" for SHORTCUT_NONE
char const *shortcut_name(shortcut_t s);

// Switch app (✕): the last two apps opened, as lichen_app_at() indexes, -1 for none
typedef struct {
    int cur;  // the app opened last
    int prev; // the one before it
} app_switch_t;

#define APP_SWITCH_INIT ((app_switch_t){-1, -1})

// An app was opened (from home, a shortcut, Settings, a notification ...)
void app_switch_opened(app_switch_t *s, int index);
// Which app ✕ opens, given the app showing now (-1: none, e.g. home): from inside the last app the one before it, from
// anywhere else the last one. -1 when there is none to go to.
int  app_switch_target(app_switch_t const *s, int showing);
// The app showing: the top-most app screen in the stack (screen ids from nav_at(), bottom first; an app's screen id is
// 1 + its index, as shell_open_app gives it). -1 when no app screen is in the stack.
int  shortcut_top_app(int const *ids, int depth, int app_count);
