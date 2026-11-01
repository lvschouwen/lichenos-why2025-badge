#pragma once

#include "lvgl.h"
#include "mesh_contact_fmt.h"

// The MeshCore app's screens (#88), between its files. LVGL task only; each list opens on the shell's stack, so Esc
// from it comes back to the grid, and Esc from the grid leaves the app.

// The app's home grid on its own screen (meshcore_grid.c): the screen the shell made for the app, its key group
void meshcore_grid_build(lv_obj_t *screen, lv_group_t *group);
// A tile's drawing (meshcore_grid_icons.c), mesh_grid_tile_t; size_px square, on its own object in parent
void meshcore_grid_icon(lv_obj_t *parent, int tile, int size_px);

// Chat: the conversation list, Enter opens the chat (meshcore_chats.c, #48)
void meshcore_chats_open(void);
// Contacts, Repeaters, Heard: one contact list, Enter opens the contact's details (meshcore_contacts.c, #49)
void meshcore_contacts_open(mesh_contact_view_t view);
// Settings: the app's settings page with its about rows, as Settings > MeshCore shows it (meshcore_app.c, #17)
void meshcore_app_settings_open(void);
