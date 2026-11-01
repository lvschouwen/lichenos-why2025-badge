// One channel's chat with a compose line (#46): see meshcore_chat.h. Everything with an lv_obj_t runs in the LVGL task;
// MeshCore is read through mesh.h copies and written through mesh_send_channel (neither blocks on the mesh task).
#include "meshcore_chat.h"

#include "esp_heap_caps.h"
#include "fonts.h"
#include "lichen_clock.h"
#include "list_filter.h"
#include "menu.h"
#include "menu_filter.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_frame.h"
#include "mesh_format.h"
#include "lichen_ui.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SCREEN_CHAT    142
#define SHOW_MESSAGES  50
#define SCREEN_WAIT_MS 20
#define SCROLL_STEP_PX 120
#define AGE_REFRESH_S  30
#define TICK_MS        500
#define SCREEN_DM_PICK 143
#define SCREEN_DM      144
#define MAX_PEERS      100
#define ADV_TYPE_CHAT  1 // MeshCore: a person's node (repeaters, rooms and sensors have no DMs)

static char    channel[MESH_NAME_MAX + 2]; // the chat that is open: a channel name, or the contact's name in a DM
static bool    dm_mode;                       // #47: a chat with one contact
static uint8_t peer[MF_PREFIX];               // that contact's key prefix
static size_t  copy_n;
static lv_obj_t *tab_hop; // an empty second focusable: Tab moves the focus onto it, which retries the failed message

#define HINT_CHANNEL "Enter: send    /: search    Esc: back"
#define HINT_DM      "Enter: send    Tab: retry a failed one    /: search    Esc: back"
#define HINT_SEARCH  "Up/Down: match    Enter: done    Esc: cancel" // fits one line at 720 px
#define HIT_BORDER_W 4 // on every row, shown only on the current hit: a border that came and went would move the text

static lv_obj_t   *list, *ta, *counter, *note;
static lv_timer_t *timer;
static mesh_msg_t *copy;
static uint32_t    shown_generation, shown_s;
static bool        note_error;

// Search inside the chat (#48): the line that takes the compose line's place, and the hits among copy[]
static lv_obj_t *search_line;
static bool      searching;
static char      query[LIST_FILTER_MAX + 1];
static uint16_t  hits[SHOW_MESSAGES];
static size_t    hit_n, hit_pos;
static bool      have_hit;      // hit_seq names the current hit: it survives a rebuild by seq, not by index
static uint32_t  hit_seq;
static int32_t   search_from_y; // where the list was when the search began: Esc goes back there
static bool      search_from_end; // ... or it was at the end: Esc goes to the end, messages may have come in since

static void add_line(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
}

static void add_message(mesh_msg_t const *m, uint32_t now, uint32_t now_epoch) {
    char age[24], status[32], meta[96];
    mesh_format_msg_age(m->loaded, m->heard_s, m->epoch, now, now_epoch, age, sizeof(age));
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
    lv_obj_set_style_pad_hor(item, 8, 0);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(item, HIT_BORDER_W, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_border_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(item, false);
    lv_obj_set_clickable(item, false);
    add_line(item, m->text, m->outgoing ? THEME_AMBER : THEME_TEXT);
    add_line(item, meta, color);
}

static void search_apply(bool scroll);

static void rebuild(size_t n) {
    bool at_end = lv_obj_get_scroll_bottom(list) <= 0;
    int32_t y   = lv_obj_get_scroll_y(list);
    lv_obj_clean(list);
    copy_n = n;
    size_t shown = 0;
    uint32_t now = mesh_uptime_s(), now_epoch = mesh_epoch_now();
    for (size_t i = 0; i < n; i++) {
        add_message(&copy[i], now, now_epoch);
        shown++;
    }
    if (!shown) {
        add_line(list, "No messages here yet.", THEME_DIM);
    }
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, at_end ? LV_COORD_MAX : y, LV_ANIM_OFF);
    if (searching) {
        search_apply(false); // new rows: highlight them again, the view stays where it is
    }
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

static void set_default_note(void);

static void set_note(char const *text, bool error) {
    lv_label_set_text(note, text);
    lv_obj_set_style_text_color(note, lv_color_hex(error ? THEME_RED : THEME_DIM), 0);
    note_error = error;
}

// The usual line under the composer; the clock hint takes its place while the clock is not set (#8) or only
// approximate (#83)
static void set_default_note(void) {
    char const *clock = mesh_clock_hint(clock_is_set(), clock_is_approximate());
    set_note(clock ? clock : (dm_mode ? HINT_DM : HINT_CHANNEL), false);
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
    set_default_note();
    show_counter();
    shown_generation = UINT32_MAX; // the mesh task adds it within a few ms: the next tick shows it
}

static void on_changed(lv_event_t *e) {
    (void)e;
    show_counter();
    if (note_error) {
        set_default_note();
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

// ---- search inside the chat (#48) -----------------------------------------------------------------------------------

// Highlights the hits (THEME_TILE) and frames the current one (amber left border); scroll brings the current one into
// view. Works on copy[], the snapshot the screen already holds: no second mesh lock.
static void search_apply(bool scroll) {
    // copy[] may be a new snapshot already (rebuild): resolve the current hit by its seq, never by an old index
    hit_n    = mesh_chat_rehit(copy, copy_n, query, hits, SHOW_MESSAGES, have_hit, hit_seq, &hit_pos);
    have_hit = hit_pos < hit_n;
    hit_seq  = have_hit ? copy[hits[hit_pos]].seq : 0;
    size_t h = 0;
    for (size_t i = 0; i < copy_n; i++) {
        lv_obj_t *item = lv_obj_get_child(list, (int32_t)i);
        bool      hit  = h < hit_n && hits[h] == i;
        if (!item) {
            break;
        }
        lv_obj_set_style_bg_opa(item, hit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(item, hit && h == hit_pos ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        h += hit;
    }
    char text[96];
    mesh_chat_search_line(query, hit_pos, hit_n, text, sizeof(text));
    lv_label_set_text(search_line, text);
    if (scroll && hit_pos < hit_n) {
        lv_obj_t *item = lv_obj_get_child(list, hits[hit_pos]);
        if (item) {
            lv_obj_update_layout(list);
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        }
    }
}

static void search_enter(void) {
    lv_group_t *group = lv_obj_get_group(ta);
    if (!group || !search_line || searching) {
        return;
    }
    searching     = true;
    query[0]      = '\0';
    hit_n         = 0;
    hit_pos         = MESH_CHAT_NO_HIT;
    have_hit        = false;
    search_from_y   = lv_obj_get_scroll_y(list);
    search_from_end = lv_obj_get_scroll_bottom(list) <= 0;
    // The search line takes the focus (and the keys) before the compose line hides: hiding a focused object moves the
    // focus on, which in a DM is the Tab-retry hop
    lv_group_add_obj(group, search_line);
    lv_obj_remove_flag(search_line, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(search_line);
    lv_obj_add_flag(ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(counter, LV_OBJ_FLAG_HIDDEN);
    set_note(HINT_SEARCH, false);
    search_apply(false);
}

// Enter keeps the view where the search took it; Esc (restore) goes back to where the search began
static void search_leave(bool restore) {
    if (!searching) {
        return;
    }
    searching = false;
    query[0]  = '\0';
    search_apply(false); // no query, no hits: every row plain again
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(counter, LV_OBJ_FLAG_HIDDEN);
    lv_group_focus_obj(ta);
    lv_group_remove_obj(search_line);
    lv_obj_add_flag(search_line, LV_OBJ_FLAG_HIDDEN);
    set_default_note();
    if (restore) {
        lv_obj_update_layout(list);
        lv_obj_scroll_to_y(list, search_from_end ? LV_COORD_MAX : search_from_y, LV_ANIM_OFF);
    }
}

// The compose line sees a key before the textarea does: "/" on an empty line starts a search and never reaches the
// draft
static void on_compose_key(lv_event_t *e) {
    if (lv_event_get_key(e) == '/' && !lv_textarea_get_text(ta)[0]) {
        lv_event_stop_processing(e);
        search_enter();
    }
}

// Keys while searching (the search line has the focus): typing and Backspace edit the query, Up/Down jump between
// hits, Enter is done
static void on_search_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (!searching) {
        return;
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (hit_n == 0) {
            return;
        }
        hit_pos  = mesh_chat_hit_step(hit_pos, hit_n, key == LV_KEY_UP ? -1 : 1);
        hit_seq  = copy[hits[hit_pos]].seq; // hits[] matches copy[]: both were set by the last search_apply
        have_hit = true;
        search_apply(true);
    } else if (key == LV_KEY_ENTER) {
        search_leave(false);
    } else if (list_filter_edit(query, sizeof(query), key)) {
        have_hit = false; // a new query starts at its newest hit
        search_apply(true);
    }
}

static void on_focus_move(lv_group_t *group) {
    if (searching && lv_group_get_focused(group) != search_line) {
        lv_group_focus_obj(search_line); // Tab does nothing while searching (no retry from a hidden compose line)
        return;
    }
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

// Esc leaves a search first (#48), then clears a draft (#57), then leaves
static bool on_esc(void *arg) {
    (void)arg;
    if (searching) {
        search_leave(true);
        return true;
    }
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
    // The focus callback was set on this screen's group; the shell usually deletes the group first (then ta has none)
    lv_group_t *group = ta ? lv_obj_get_group(ta) : NULL;
    if (group) {
        lv_group_set_focus_cb(group, NULL);
    }
    heap_caps_free(copy);
    copy = NULL;
    list = ta = counter = note = tab_hop = search_line = NULL;
    searching = false;
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

// The search key (●, #79), as "/" on an empty compose line
static void on_search(void *arg) {
    (void)arg;
    search_enter(); // does nothing while a search is open
}

static lv_obj_t *build_chat(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    shell_on_esc(on_esc, NULL);
    shell_on_search(on_search, NULL);

    lv_obj_t *heading = lv_label_create(screen);
    lv_obj_set_width(heading, LV_PCT(100));
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lv_obj_set_style_text_color(heading, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(heading, channel); // "@bob" for a DM, as the Chat list's rows (#101)

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
    lv_obj_add_event_cb(ta, on_compose_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, NULL);

    // Takes the compose line's place while searching (#48), styled like it so nothing else moves
    search_line = lv_label_create(screen);
    lv_obj_set_width(search_line, LV_PCT(100));
    lv_label_set_long_mode(search_line, LV_LABEL_LONG_DOT);
    lv_obj_set_style_bg_color(search_line, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(search_line, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(search_line, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(search_line, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_radius(search_line, 14, 0);
    lv_obj_set_style_pad_all(search_line, 14, 0);
    lv_obj_set_style_border_width(search_line, 3, 0);
    lv_obj_set_style_border_color(search_line, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_outline_width(search_line, 0, LV_STATE_ANY);
    lv_obj_add_flag(search_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(search_line, on_search_key, LV_EVENT_KEY, NULL);

    counter = lv_label_create(screen);
    lv_obj_set_width(counter, LV_PCT(100));
    note = lv_label_create(screen);
    lv_obj_set_width(note, LV_PCT(100));
    set_default_note();
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
        lv_group_focus_obj(ta);
    }
    if (group) {
        lv_group_set_focus_cb(group, on_focus_move); // the screen's own group; keeps the focus on a search (#48)
    }
    searching = false;
    copy_n           = 0;
    copy             = heap_caps_malloc(SHOW_MESSAGES * sizeof(*copy), MALLOC_CAP_SPIRAM);
    shown_generation = UINT32_MAX;
    if (copy) {
        timer = lv_timer_create(tick, TICK_MS, NULL);
        tick(timer);
    }
    return screen;
}

// ---- a channel chat, from the Chat list (#48) ------------------------------------------------------------------------

static void open_chat(void *arg) {
    snprintf(channel, sizeof(channel), "%s", (char const *)arg);
    dm_mode = false;
    shell_open(build_chat, SCREEN_CHAT);
}

void meshcore_chat_open_channel(char const *name) {
    char copy_name[sizeof(channel)]; // name may be a row of a list that is rebuilt while the chat opens
    snprintf(copy_name, sizeof(copy_name), "%s", name);
    open_chat(copy_name);
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

void meshcore_chat_open_dm(uint8_t const prefix[6], char const *title) {
    snprintf(channel, sizeof(channel), "%s%s", title[0] == '@' ? "" : "@", title);
    memcpy(peer, prefix, MF_PREFIX);
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
