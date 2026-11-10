#pragma once

#include "lichen_sys_net.h"
#include "lichen_ui.h"
#include "link_state.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Lichen shell (#66): boot screen, home, a screen stack with Esc back to home, the status bar. All functions take
// the LVGL port lock themselves and may be called from any task. The calls apps use are in lichen_ui.h (#84).

// What the shell shows that only a layer above it knows (#284): everything else it reads through lichen_sys_*.h. May be
// NULL members; polled from the LVGL task, so quick and never waiting on a lock that may be held for long.
typedef struct {
    lora_use_t (*lora_use)(void); // what the radio's user (MeshCore) does (#103, #105); NULL = always running
} shell_sources_t;

void shell_set_sources(shell_sources_t const *sources); // before shell_start; copied
// Shows the boot screen; it moves on to home by itself 2 s after the apps step ends
void shell_start(lv_display_t *display);
// The boot screen alone, as early as the display is up (#39): before storage, notices, assets and the fonts, so with
// LVGL's built-in fonts. shell_start (later) then only adds the rest. Optional: shell_start does it when it has not run.
void shell_boot_screen(lv_display_t *display);
// A "Restarting" frame (#39), drawn synchronously right before a soft restart and held ~0.7 s so it can be read (#134). Any task; gives
// up after 300 ms if LVGL is busy (a restart must never wait on the UI).
void shell_show_restarting(void);
// A line for the restart frame and the goodbye screen of the restart that follows ("The screen stays dark for up to a
// minute"): what the user must know before the panel goes dark (R10, #138). NULL clears it; copied. Any task.
void shell_restart_note(char const *text);
char const *shell_restart_note_get(void);
// The keyboard comes up after the C6 (C6 EN resets its chip), so later than the screen
void shell_attach_keys(lv_indev_t *keys);
// Update mode (#76): deletes every screen below the top one, home included, so their LVGL objects are freed. The top
// one becomes the only frame; Esc and shell_open do nothing afterwards (there is no home to go back to).
void shell_close_all_but_top(void);
// The Power app (#65): what the OS does once the apps stopped and the saves are done, on the power task. power_off
// true: backlight, panel, C6 and P4 down; false: restart. Must not return. Without a hook the badge only restarts.
void shell_set_power_hook(void (*finish)(bool power_off));
// Every soft restart (system_restart's save hook, #123): the notifications are saved, then the apps stop and save as
// for an update (MeshCore: its files, the message journal flushed). Blocks for seconds; fw_console runs it on a task of
// its own with a bound. Safe without a screen, and again after the Power app or an update already stopped the apps.
void shell_save_for_restart(void);
// The factory reset key combo (#66, factory_combo.c): right after keyboard_start; watches the keys for 10 s
void factory_combo_start(void);
// The boot screen is up (the OTA valid gate: a new image must show it before it counts as working)
bool shell_up(void);
// Home is on screen and its fade-in is over (screens may open; the OS watch services' notices wait for it, #281)
bool shell_home_shown(void);
// The thermal watchdog's hooks (#281, main hands them to lichen_watch_start). battery_open_alert: Settings > Battery
// headed by the heat warning; power_request_off: Power > Power off without the question (the goodbye screen, the
// saves, then the power hook). Both from any task.
void battery_open_alert(void);
void power_request_off(void);
// Restart or Power off is under way (the goodbye screen is up); any task
bool power_going_down(void);

// The countdown dialog (shell_dialog_countdown, #14 T-B) is in lichen_ui.h: apps use it too (#49)
