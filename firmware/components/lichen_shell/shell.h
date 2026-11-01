#pragma once

#include "lichen_ui.h"
#include "lvgl.h"

#include "wifi_join.h"
#include "wifi_list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Lichen shell (#66): boot screen, home, a screen stack with Esc back to home, the status bar. All functions take
// the LVGL port lock themselves and may be called from any task. The calls apps use are in lichen_ui.h (#84).

// What the shell shows about the rest of the badge, read from the LVGL task once a second: each must be quick and
// never wait on a lock that may be held for long. Any member may be NULL.
typedef struct {
    bool (*wifi_up)(void);
    bool (*lora_up)(void);                  // the LoRa radio runs (and the C6 link is up)
    bool (*c6_up)(void);                    // the C6 link: brightness and the radio need it
    // Settings > Wi-Fi: wifi_scan blocks for seconds and only runs off the LVGL task; wifi_join saves and applies
    bool (*wifi_scan)(wifi_ap_t *out, size_t max, size_t *n);
    bool (*wifi_ip)(char *ip, size_t cap); // false while there is no IP
    wifi_join_state_t (*wifi_join_state)(char *tried, size_t tried_cap, char *kept, size_t kept_cap);
    bool (*wifi_join)(char const *ssid, char const *pass);
    bool (*wifi_rssi)(int8_t *out);     // signal of the connected AP, false while unknown (status bar bars)
    bool (*wifi_joining)(void);         // a join is under way (status bar amber)
    bool (*wifi_has_credentials)(void); // first-run setup skips the Wi-Fi step when the badge has a network saved
    // The C6 is being recovered or is gone (#14): the state in words ("recovering 2/3", "down (no answer)"), false
    // while it is up
    bool (*c6_trouble)(char *out, size_t cap);
} shell_sources_t;

void shell_set_sources(shell_sources_t const *sources); // before shell_start; copied
// Shows the boot screen; it moves on to home by itself 2 s after the apps step ends
void shell_start(lv_display_t *display);
// The keyboard comes up after the C6 (C6 EN resets its chip), so later than the screen
void shell_attach_keys(lv_indev_t *keys);
// Update mode (#76): deletes every screen below the top one, home included, so their LVGL objects are freed. The top
// one becomes the only frame; Esc and shell_open do nothing afterwards (there is no home to go back to).
void shell_close_all_but_top(void);
// The boot screen is up (the OTA valid gate: a new image must show it before it counts as working)
bool shell_up(void);

// An OS warning dialog over every screen (#14 T-B), e.g. "C6 (radio/Wi-Fi) not responding — restarting in 30 s".
// The message reads "<title> <count_prefix><seconds left><count_suffix>", the hint below it. Enter confirms, Esc
// cancels, and when the countdown runs out it confirms by itself (the keyboard may be dead), or cancels when careful.
// done runs once on the LVGL task with the answer: keep it short (queue the work). One dialog at a time: a second
// call replaces the first. Safe from any task, never blocks on more than the LVGL lock (2 s); false when it could not
// be shown.
typedef struct {
    char const *title;
    char const *count_prefix;
    char const *count_suffix;
    char const *hint;
    uint32_t    seconds;
    bool        careful; // the countdown ends in a cancel and Enter counts only after a second (an erase, #99)
} shell_dialog_t;
typedef void (*shell_dialog_fn)(bool confirmed);
bool shell_dialog_countdown(shell_dialog_t const *d, shell_dialog_fn done);
// Closes it without an answer (the reason for it went away); safe from any task
void shell_dialog_close(void);
