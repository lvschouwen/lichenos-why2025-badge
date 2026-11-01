#pragma once

#include "lichen_ui.h"
#include "link_state.h"
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
    lora_use_t (*lora_use)(void);           // what the radio's user does (#103, #105); NULL = always running
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
    // LoRa packets received and sent so far, both only going up: the status bar's TX/RX chevrons, polled every 100 ms
    // (#29)
    void (*lora_counts)(uint32_t *rx, uint32_t *tx);
    // C6 outages the badge came back from (#14) and the last recovery in words, for the notification (#62)
    uint32_t (*c6_restarts)(char *reason, size_t cap);
} shell_sources_t;

void shell_set_sources(shell_sources_t const *sources); // before shell_start; copied
// Shows the boot screen; it moves on to home by itself 2 s after the apps step ends
void shell_start(lv_display_t *display);
// The keyboard comes up after the C6 (C6 EN resets its chip), so later than the screen
void shell_attach_keys(lv_indev_t *keys);
// Update mode (#76): deletes every screen below the top one, home included, so their LVGL objects are freed. The top
// one becomes the only frame; Esc and shell_open do nothing afterwards (there is no home to go back to).
void shell_close_all_but_top(void);
// The Power app (#65): what the OS does once the apps stopped and the saves are done, on the power task. power_off
// true: backlight, panel, C6 and P4 down; false: restart. Must not return. Without a hook the badge only restarts.
void shell_set_power_hook(void (*finish)(bool power_off));
// The boot screen is up (the OTA valid gate: a new image must show it before it counts as working)
bool shell_up(void);

// The countdown dialog (shell_dialog_countdown, #14 T-B) is in lichen_ui.h: apps use it too (#49)
