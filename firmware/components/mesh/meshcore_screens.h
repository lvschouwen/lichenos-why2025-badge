#pragma once

#include "lvgl.h"
#include "mesh_contact_fmt.h"

// The MeshCore app's screens (#88), between its files. LVGL task only; each list opens on the shell's stack, so Esc
// from it comes back to the grid, and Esc from the grid leaves the app.

// The app's home grid on its own screen (meshcore_grid.c): the screen the shell made for the app, its key group
void meshcore_grid_build(lv_obj_t *screen, lv_group_t *group);
// Arrange mode (#189): Settings > Arrange tiles (meshcore_settings.c). _possible: the grid is on the stack (a Settings page
// opened from its tile); _start closes the screens above it and starts arranging, false when it could not be queued.
// _reload shows the saved order again (after a reset). _esc is the shell_on_esc hook the grid's builder sets.
bool meshcore_grid_arrange_possible(void);
bool meshcore_grid_arrange_start(void);
void meshcore_grid_reload(void);
bool meshcore_grid_esc(void *arg);
// A tile's drawing (meshcore_grid_icons.c), mesh_grid_tile_t; size_px square, on its own object in parent
void meshcore_grid_icon(lv_obj_t *parent, int tile, int size_px);

// Chat: the conversation list, Enter opens the chat (meshcore_chats.c, #48)
void meshcore_chats_open(void);
// Contacts, Repeaters, Heard: one contact list, Enter opens the contact's details (meshcore_contacts.c, #49)
void meshcore_contacts_open(mesh_contact_view_t view);
// Settings: the app's settings page with its about rows, as Settings > MeshCore shows it (meshcore_app.c, #17)
void meshcore_app_settings_open(void);
// Notifications (#50): the page that sets what MeshCore sends (Settings > Notifications > MeshCore, and under Settings >
// MeshCore), and the opener for a notice's target (meshcore_notify.c)
void meshcore_notify_settings_open(void);
void meshcore_open_notification(char const *target, uint32_t id);
// Radio health (#157): MeshCore's counters and the radio's channel reading, read only (meshcore_health.c). Opened from
// the app's Settings page; a menu row callback.
void meshcore_health_open(void *arg);
// A contact's Signal page (#166): graphs of the RSSI and SNR of its adverts and messages heard since start, the hop count
// and the last advert path (meshcore_signal.c). Opened from the contact's page; receive only. prefix: 6 key bytes.
void meshcore_signal_open(uint8_t const prefix[6], char const *name);
// A repeater's Status history (#168): the switch for its scheduled status asks (on air: asks first), the interval and
// small graphs of the answers kept since start (meshcore_rhist.c). Opened from the repeater's page; opening it sends
// nothing.
void meshcore_rhist_open(uint8_t const prefix[6], char const *name);
// Discover nearby (#167): one zero-hop request after a careful confirm, then who answered with SNR (meshcore_discover.c).
// Opened from the app's Settings page; a menu row callback. Opening it sends nothing.
void meshcore_discover_open(void *arg);
// Contact radar (#171): contacts that advertise a position, by distance and bearing from our own (meshcore_radar.c).
void meshcore_radar_open(void *arg);
// Air log (#165): the received packets, headers only, newest first (meshcore_airlog.c). Opened from the app's Settings
// page; a menu row callback.
void meshcore_airlog_open(void *arg);
// The applied radio (#335, meshcore_applied.c): "LoRa radio now: ..." from the registry cache and, while it differs
// from MeshCore's settings, an amber "Differs: MeshCore asks for ..." note; both refresh each second while the menu is up
void meshcore_applied_notes(lv_obj_t *menu);
