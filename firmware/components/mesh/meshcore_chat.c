// One channel's chat with a compose line (#46): see meshcore_chat.h. Everything with an lv_obj_t runs in the LVGL task;
// MeshCore is read through mesh.h copies and written through mesh_send_channel (neither blocks on the mesh task).
#include "meshcore_chat.h"

#include "esp_heap_caps.h"
#include "fonts.h"
#include "list_filter.h"
#include "menu.h"
#include "menu_filter.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_frame.h"
#include "mesh_format.h"
#include "shell.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_PICK    141
#define SCREEN_CHAT    142
#define SHOW_MESSAGES  50
#define SCREEN_WAIT_MS 20
#define SCROLL_STEP_PX 120
#define AGE_REFRESH_S  30
#define TICK_MS        500
#define MAX_CHANNELS   40
#define SCREEN_DM_PICK 143
#define SCREEN_DM      144
#define MAX_PEERS      100
#define ADV_TYPE_CHAT  1 // MeshCore: a person's node (repeaters, rooms and sensors have no DMs)

static char    channel[MESH_NAME_MAX + 2]; // the chat that is open: a channel name, or the contact's name in a DM
static bool    dm_mode;                       // #47: a chat with one contact
static uint8_t peer[MF_PREFIX];               // that contact's key prefix
static size_t  copy_n;
static lv_obj_t *tab_hop; // an empty second focusable: Tab moves the focus onto it, which retries the failed message

#define HINT_CHANNEL "Enter: send    Esc: back"
#define HINT_DM      "Enter: send    Tab: retry a failed one    Esc: back"

static lv_obj_t   *list, *ta, *counter, *note;
static lv_timer_t *timer;
static mesh_msg_t *copy;
static uint32_t    shown_generation, shown_s;
static bool        note_error;

static void add_line(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
}

static void add_message(mesh_msg_t const *m, uint32_t now) {
    char age[24], status[32], meta[96];
    mesh_format_age(now - m->heard_s, age, sizeof(age));
    mesh_status_kind_t kind = mesh_msg_status(m, status, sizeof(status));
    uint32_t color = kind == MESH_STATUS_FAILED ? THEME_RED : kind == MESH_STATUS_OK ? THEME_TEAL : THEME_DIM;
    if (m->outgoing) {
        snprintf(meta, sizeof(meta), "%s, %s", status, age);
    } else {
        snprintf(meta, sizeof(meta), "%s, %d dBm", age, m->rssi_x10 / 10);
    }
    lv_obj_t *item = lv_obj_create(list);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(item, 4, 0);
    lv_obj_set_scrollable(item, false);
    lv_obj_set_clickable(item, false);
    add_line(item, m->text, m->outgoing ? THEME_AMBER : THEME_TEXT);
    add_line(item, meta, color);
}

static void rebuild(size_t n) {
    bool at_end = lv_obj_get_scroll_bottom(list) <= 0;
    int32_t y   = lv_obj_get_scroll_y(list);
    lv_obj_clean(list);
    copy_n = n;
    size_t shown = 0;
    uint32_t now = mesh_uptime_s();
    for (size_t i = 0; i < n; i++) {
        add_message(&copy[i], now);
        shown++;
    }
    if (!shown) {
        add_line(list, "No messages here yet.", THEME_DIM);
    }
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, at_end ? LV_COORD_MAX : y, LV_ANIM_OFF);
}

static size_t text_max(void) {
    return dm_mode ? MF_MAX_TEXT : mesh_channel_text_max();
}

static void show_counter(void) {
    char text[80];
    mesh_compose_status(lv_textarea_get_text(ta), text_max(), text, sizeof(text));
    lv_label_set_text(counter, text);
}

static void tick(lv_timer_t *t) {
    (void)t;
    uint32_t        generation;
    mesh_chat_key_t key;
    if (dm_mode) {
        mesh_chat_key_dm(&key, peer);
    } else {
        mesh_chat_key_channel(&key, channel);
    }
    // The chat's own ring (#59), read with mark_read: the chat is on screen, so it counts as read (#48)
    size_t n = mesh_chat_snapshot(&key, copy, SHOW_MESSAGES, &generation, true, SCREEN_WAIT_MS);
    if (n == MESH_BUSY) {
        return;
    }
    if (generation != shown_generation || mesh_uptime_s() - shown_s >= AGE_REFRESH_S) {
        rebuild(n);
        shown_generation = generation;
        shown_s          = mesh_uptime_s();
    }
}

static void set_note(char const *text, bool error) {
    lv_label_set_text(note, text);
    lv_obj_set_style_text_color(note, lv_color_hex(error ? THEME_RED : THEME_DIM), 0);
    note_error = error;
}

static void on_ready(lv_event_t *e) {
    (void)e;
    char const *text = lv_textarea_get_text(ta);
    if (!text[0]) {
        return;
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = dm_mode ? mesh_send_dm(peer, text, &why) : mesh_send_channel(channel, text, &why);
    if (!id) {
        set_note(why ? why : "not sent", true);
        return;
    }
    lv_textarea_set_text(ta, "");
    set_note(dm_mode ? HINT_DM : HINT_CHANNEL, false);
    show_counter();
    shown_generation = UINT32_MAX; // the mesh task adds it within a few ms: the next tick shows it
}

static void on_changed(lv_event_t *e) {
    (void)e;
    show_counter();
    if (note_error) {
        set_note(dm_mode ? HINT_DM : HINT_CHANNEL, false);
    }
}

// Tab: start over with the newest failed DM of this chat
static void retry_failed(void) {
    for (size_t i = copy_n; i > 0; i--) {
        mesh_msg_t const *m = &copy[i - 1];
        if (m->outgoing && mesh_msg_in_dm(m, peer) && m->state == MESH_OUT_FAILED) {
            if (mesh_retry(m->id)) {
                set_note("Trying again", false);
                shown_generation = UINT32_MAX;
            } else {
                set_note("Cannot retry now", true);
            }
            return;
        }
    }
    set_note("Nothing failed to retry", false);
}

static void on_focus_move(lv_group_t *group) {
    if (tab_hop && lv_group_get_focused(group) == tab_hop) {
        retry_failed();
        lv_group_focus_obj(ta); // calls this again, with the compose line focused: a no-op
    }
}

static void on_key(lv_event_t *e) {
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP: shell_scroll_by(list, SCROLL_STEP_PX); break;
        case LV_KEY_DOWN: shell_scroll_by(list, -SCROLL_STEP_PX); break;
        default: break;
    }
}

// Esc clears a draft first, then leaves (#57)
static bool clear_draft(void *arg) {
    (void)arg;
    if (!ta || !lv_textarea_get_text(ta)[0]) {
        return false;
    }
    lv_textarea_set_text(ta, "");
    return true;
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(copy);
    copy = NULL;
    list = ta = counter = note = tab_hop = NULL;
}

static void style_ta(void) {
    lv_obj_set_width(ta, LV_PCT(100));
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(ta, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_radius(ta, 14, 0);
    lv_obj_set_style_pad_all(ta, 14, 0);
    lv_obj_set_style_border_width(ta, 3, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_outline_width(ta, 0, LV_STATE_ANY);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, LV_PART_CURSOR);
    lv_obj_set_style_bg_color(ta, lv_color_hex(THEME_AMBER), LV_PART_CURSOR);
    lv_obj_set_style_text_color(ta, lv_color_hex(THEME_BG), LV_PART_CURSOR);
}

static lv_obj_t *build_chat(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    shell_on_esc(clear_draft, NULL);

    lv_obj_t *heading = lv_label_create(screen);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(heading, dm_mode ? channel + (channel[0] == '@') : channel);

    list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_ACTIVE);
    shell_scroll_tame(list);

    ta = lv_textarea_create(screen);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, (uint32_t)text_max());
    lv_textarea_set_placeholder_text(ta, "Write a message");
    style_ta();
    lv_obj_add_event_cb(ta, on_ready, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(ta, on_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(ta, on_key, LV_EVENT_KEY, NULL);

    counter = lv_label_create(screen);
    lv_obj_set_width(counter, LV_PCT(100));
    note = lv_label_create(screen);
    lv_obj_set_width(note, LV_PCT(100));
    set_note(dm_mode ? HINT_DM : HINT_CHANNEL, false);
    lv_obj_set_style_text_color(counter, lv_color_hex(THEME_DIM), 0);
    show_counter();

    lv_group_t *group = lv_group_get_default();
    tab_hop           = NULL;
    if (dm_mode && group) {
        tab_hop = lv_obj_create(screen);
        lv_obj_remove_style_all(tab_hop);
        lv_obj_set_size(tab_hop, 0, 0);
        lv_obj_set_floating(tab_hop, true);
        lv_obj_set_clickable(tab_hop, false);
        lv_group_add_obj(group, tab_hop);
        lv_group_set_focus_cb(group, on_focus_move);
        lv_group_focus_obj(ta);
    }
    copy_n           = 0;
    copy             = heap_caps_malloc(SHOW_MESSAGES * sizeof(*copy), MALLOC_CAP_SPIRAM);
    shown_generation = UINT32_MAX;
    if (copy) {
        timer = lv_timer_create(tick, TICK_MS, NULL);
        tick(timer);
    }
    return screen;
}

// ---- the channel picker ----------------------------------------------------------------------------------------------

static char names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];

static void open_chat(void *arg) {
    snprintf(channel, sizeof(channel), "%s", (char const *)arg);
    dm_mode = false;
    shell_open(build_chat, SCREEN_CHAT);
}

static bool word_in_name(char const *word, void *name) {
    return list_filter_text_has(name, word);
}

static bool name_matches(char const *name, char const *query) {
    return list_filter_match(query, word_in_name, (void *)name);
}

static bool channel_matches(size_t i, char const *query, void *ctx) {
    (void)ctx;
    return name_matches(names[i], query);
}

static lv_obj_t *build_pick(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Write on a channel");
    menu_filter_t *filter = menu_filter_create(screen, menu, channel_matches, NULL); // #57
    size_t    n      = mesh_channels_snapshot_wait(names, MAX_CHANNELS, SCREEN_WAIT_MS);
    if (n == MESH_BUSY) {
        menu_note(menu, "MeshCore is busy, press Esc and try again");
    } else if (n == 0) {
        menu_note(menu, "No channels (MeshCore not running?)");
    }
    for (size_t i = 0; i < n && n != MESH_BUSY; i++) {
        menu_filter_add(filter, menu_add(menu, names[i], "", open_chat, names[i]), i);
    }
    return screen;
}

void meshcore_chat_pick(void) {
    shell_open(build_pick, SCREEN_PICK);
}

// ---- the contact picker, then the DM chat (#47) --------------------------------------------------------------------

static mesh_contact_t *peers;

static void open_dm(void *arg) {
    mesh_contact_t const *c = arg;
    snprintf(channel, sizeof(channel), "@%s", c->name[0] ? c->name : "(no name)");
    memcpy(peer, c->pubkey, MF_PREFIX);
    dm_mode = true;
    shell_open(build_chat, SCREEN_DM);
}

static void free_peers(lv_event_t *e) {
    (void)e;
    heap_caps_free(peers);
    peers = NULL;
}

// A word matches the name, or (hex digits) the start of the key
static bool word_in_contact(char const *word, void *arg) {
    mesh_contact_t const *c = arg;
    return list_filter_text_has(c->name[0] ? c->name : "(no name)", word) ||
           list_filter_key_starts(c->pubkey, sizeof(c->pubkey), word);
}

static bool contact_matches(size_t i, char const *query, void *ctx) {
    (void)ctx;
    return list_filter_match(query, word_in_contact, &peers[i]);
}

static lv_obj_t *build_dm_pick(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Message a contact");
    menu_filter_t *filter = menu_filter_create(screen, menu, contact_matches, NULL); // #57
    lv_obj_add_event_cb(screen, free_peers, LV_EVENT_DELETE, NULL);
    heap_caps_free(peers);
    peers = heap_caps_malloc(MAX_PEERS * sizeof(*peers), MALLOC_CAP_SPIRAM);
    size_t total, n = peers ? mesh_contacts_snapshot(peers, MAX_PEERS, &total, SCREEN_WAIT_MS) : 0;
    if (!peers || n == MESH_BUSY) {
        menu_note(menu, "MeshCore is busy, press Esc and try again");
        return screen;
    }
    size_t shown = 0;
    for (size_t i = 0; i < n; i++) {
        if (peers[i].type == ADV_TYPE_CHAT) {
            menu_filter_add(filter, menu_add(menu, peers[i].name[0] ? peers[i].name : "(no name)", "", open_dm, &peers[i]), i);
            shown++;
        }
    }
    if (!shown) {
        menu_note(menu, "No contacts to message yet");
    }
    return screen;
}

void meshcore_chat_pick_contact(void) {
    shell_open(build_dm_pick, SCREEN_DM_PICK);
}
