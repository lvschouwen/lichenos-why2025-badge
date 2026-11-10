// MeshCore's notification settings page and the notice opener (#50). LVGL task only. The same page opens from Settings ›
// Notifications › MeshCore (the shell calls notify_settings_open) and from Settings › MeshCore › Notifications. The
// OS's own per-app switch stays in the centre's settings (lichen_notify_source_enabled); these rows are what MeshCore
// sends once that is on.
#include "esp_attr.h"
#include "esp_log.h"
#include "screen_ids.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_notify_run.h"
#include "meshcore_chat.h"
#include "meshcore_screens.h"
#include "meshcore_settings.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_CHANNELS       40 // MAX_GROUP_CHANNELS in the mesh component's build
#define SCREEN_WAIT_MS     20

static lv_obj_t *sw_row[3];
static lv_obj_t *chan_row[MAX_CHANNELS];
EXT_RAM_BSS_ATTR static char      chan_name[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];
static size_t    chan_n;

static char const *on_off(bool on) {
    return on ? "On" : "Off";
}

static void toggle_switch(void *arg) {
    mn_switch_t which = (mn_switch_t)(uintptr_t)arg;
    mesh_notify_set_switch(which, !mesh_notify_get_switch(which));
    menu_set_value(sw_row[which], on_off(mesh_notify_get_switch(which)));
}

static void cycle_channel(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;
    if (i >= chan_n) {
        return;
    }
    mn_mode_t next = (mn_mode_t)((mesh_notify_chan_mode(chan_name[i]) + 1) % MN_MODE_COUNT);
    mesh_notify_set_chan_mode(chan_name[i], next);
    menu_set_value(chan_row[i], mn_mode_name(next));
}

static void on_page_delete(lv_event_t *e) {
    (void)e;
    memset(sw_row, 0, sizeof(sw_row));
    memset(chan_row, 0, sizeof(chan_row));
    chan_n = 0;
}

static lv_obj_t *build_page(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "MeshCore notifications");
    lv_obj_add_event_cb(screen, on_page_delete, LV_EVENT_DELETE, NULL);
    sw_row[MN_SW_DM]       = menu_add(menu, "Direct messages", on_off(mesh_notify_get_switch(MN_SW_DM)), toggle_switch,
                                      (void *)(uintptr_t)MN_SW_DM);
    sw_row[MN_SW_FAIL]     = menu_add(menu, "Delivery failed", on_off(mesh_notify_get_switch(MN_SW_FAIL)),
                                      toggle_switch, (void *)(uintptr_t)MN_SW_FAIL);
    sw_row[MN_SW_MENTIONS] = menu_add(menu, "Mentions of my name", on_off(mesh_notify_get_switch(MN_SW_MENTIONS)),
                                      toggle_switch, (void *)(uintptr_t)MN_SW_MENTIONS);
    for (int i = 0; i < 3; i++) {
        menu_set_kind(sw_row[i], MENU_TOGGLE);
    }
    menu_note(menu, "Channels (Enter: All, Mentions, Off):");
    size_t n = mesh_channels_snapshot_wait(chan_name, MAX_CHANNELS, SCREEN_WAIT_MS);
    if (n == MESH_BUSY) {
        menu_note(menu, "MeshCore is busy or not running: open this page again to set channels.");
    } else {
        chan_n = n;
        for (size_t i = 0; i < n; i++) {
            chan_row[i] = menu_add(menu, chan_name[i], mn_mode_name(mesh_notify_chan_mode(chan_name[i])),
                                   cycle_channel, (void *)(uintptr_t)i);
            menu_set_kind(chan_row[i], MENU_CYCLE);
        }
    }
    menu_note(menu, "Mentions: @name or @[name]. Public starts on Mentions. Nothing is sent for a chat that is open; "
                    "channels on Mentions or Off do not count in the home badge. The OS switch for MeshCore is in "
                    "Settings > Notifications.");
    return screen;
}

void meshcore_notify_settings_open(void) {
    shell_open(build_page, SCREEN_NOTIFY_PAGE);
}

// ---- opening a notice -------------------------------------------------------------------------------------------------

// The notice's chat, or the conversation list when it is gone (the channel removed, the contact removed, MeshCore off)
void meshcore_open_notification(char const *target, uint32_t id) {
    (void)id;
    mesh_chat_key_t key;
    bool            failed;
    if (target && mn_target_parse(target, &key, &failed) && mesh_running()) {
        if (key.kind == MESH_CHAT_CHANNEL) {
            EXT_RAM_BSS_ATTR static char names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];
            size_t      n = mesh_channels_snapshot_wait(names, MAX_CHANNELS, SCREEN_WAIT_MS);
            int at = n == MESH_BUSY ? -1 : mn_channel_match(names, n, key.chan);
            if (at >= 0) {
                meshcore_chat_open_channel(names[at]);
                return;
            }
        } else {
            mesh_contact_detail_t d;
            if (mesh_contact_detail(key.id, &d, SCREEN_WAIT_MS) == 1) {
                char title[MESH_CONTACT_NAME_MAX + 2];
                snprintf(title, sizeof(title), "@%s", d.name[0] ? d.name : "(no name)");
                meshcore_chat_open_dm(key.id, title);
                return;
            }
        }
    }
    meshcore_chats_open(); // the shell goes back to a Chat list that is already on the stack (#148)
}
