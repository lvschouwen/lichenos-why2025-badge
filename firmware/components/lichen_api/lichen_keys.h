#pragma once
// The keyboard below LVGL for an app (#180): every TCA8418 event, the keys the OS drops before LVGL included (shape keys,
// Fn, Ctrl ...). For a screen that tests or remaps the keyboard (the Keyboard test); any other app takes keys as LVGL
// keys through its group. Implemented by the shell (lichen_shell's raw_keys.c), resolved at link time as lichen_ui.h's
// navigation. key_matrix.h names the codes and draws the printed layout.

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t code;    // 1-0x50 (key_matrix.h), without the press bit
    bool    pressed;
    bool    shift;   // a Shift key is down (for a Shift key's own event: after the event)
} lichen_raw_key_t;

typedef void (*lichen_raw_key_fn)(lichen_raw_key_t const *key, void *arg);

// LVGL context. From now on fn(key, arg) gets every key event on the LVGL task (at most about 20 ms late) and no key acts
// as a normal key, Esc included: the screen gives the user its own way out (hold_exit.h). The OS treats the screen as the
// Keyboard test meanwhile ("fw keys log" logs the codes, the shape-key shortcuts stay quiet). One app at a time: false
// when another holds the keys or the keyboard hook is not up (the keys then act as usual).
// Only for the Keyboard test today: taking the keys marks the screen as the Keyboard test for the OS (ui_remote,
// ui_probe, the shortcut filter read that flag), so a second user would need its own flag first.
bool lichen_keys_raw_take(lichen_raw_key_fn fn, void *arg);
// LVGL context: the keys act as usual again. Call it from the screen's delete event; nothing when fn/arg do not hold them.
void lichen_keys_raw_release(lichen_raw_key_fn fn, void *arg);
