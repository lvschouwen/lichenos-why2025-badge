#pragma once

#include "lvgl.h"

#include "wifi_join.h"
#include "wifi_list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Lichen shell (#66): boot screen, home, a screen stack with Esc back to home, the status bar. All functions take
// the LVGL port lock themselves and may be called from any task.

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
} shell_sources_t;

void shell_set_sources(shell_sources_t const *sources); // before shell_start; copied
// Shows the boot screen; it moves on to home by itself 2 s after the apps step ends
void shell_start(lv_display_t *display);
// The keyboard comes up after the C6 (C6 EN resets its chip), so later than the screen
void shell_attach_keys(lv_indev_t *keys);
void shell_home(void);
// Builds a screen (lv_obj_create(NULL), runs in LVGL context) and shows it on top of the stack; Esc goes back. The
// builder's focusable widgets are put in a group of their own (the default group while it runs).
void shell_open(lv_obj_t *(*build)(void), int screen_id);
// Called from inside a builder: the screen gets first use of Esc. fn(arg) runs on Esc (LVGL context); when it returns
// true the Esc is used up (a search or a draft was cleared), otherwise the screen goes back as usual (#32).
void shell_on_esc(bool (*fn)(void *arg), void *arg);
// Update mode (#76): deletes every screen below the top one, home included, so their LVGL objects are freed. The top
// one becomes the only frame; Esc and shell_open do nothing afterwards (there is no home to go back to).
void shell_close_all_but_top(void);
// Up/Down on a scrollable object: moves by dy within its content only, so nothing scrolls when it all fits and nothing
// overscrolls. shell_scroll_tame turns off LVGL's elastic and momentum scrolling on obj (screens, menus, lists).
void shell_scroll_by(lv_obj_t *obj, int32_t dy);
// A new screen styled as Settings' pages (free room for the status bar, the screen edge), for an app's own pages
// opened with shell_open(); put a menu_create() menu on it
lv_obj_t *shell_page_create(void);
void shell_scroll_tame(lv_obj_t *obj);
// The boot screen is up (the OTA valid gate: a new image must show it before it counts as working)
bool shell_up(void);
