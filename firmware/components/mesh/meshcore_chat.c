// One channel's chat with a compose line (#46): see meshcore_chat.h. Everything with an lv_obj_t runs in the LVGL task;
// MeshCore is read through mesh.h copies and written through mesh_send_channel (neither blocks on the mesh task).
#include "meshcore_chat.h"
#include "screen_ids.h"

#include "lichen_mem.h"
#include "meshcore_run.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "fonts.h"
#include "lichen_clock.h"
#include "list_filter.h"
#include "menu.h"
#include "menu_filter.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_chat_target.h"
#include "mesh_frame.h"
#include "mesh_format.h"
#include "mesh_notify_run.h"
#include "lichen_ui.h"
#include "scroll_anchor.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

#define SHOW_MESSAGES  50
#define SCREEN_WAIT_MS 20
#define SCROLL_STEP_PX 120
#define AGE_REFRESH_S  30
#define TICK_MS        500
#define MAX_PEERS      100
#define ADV_TYPE_CHAT  1 // MeshCore: a person's node (repeaters, rooms and sensors have no DMs)

#define HINT_CHANNEL "Enter: send \xE2\x80\xA2 /: search"
#define HINT_DM      "Enter: send \xE2\x80\xA2 Tab: retry a failed one \xE2\x80\xA2 /: search"
#define HINT_SEARCH  "Up/Down: match \xE2\x80\xA2 Enter: done \xE2\x80\xA2 Esc: cancel" // fits one line at 720 px
#define HIT_BORDER_W 4 // on every row, shown only on the current hit: a border that came and went would move the text

// Select mode (#111): Up on an empty compose line frames messages, Enter mentions the sender
#define HINT_SELECT "Up/Down: message \xE2\x80\xA2 Enter: reply \xE2\x80\xA2 Esc: cancel"

// One chat screen's state (#126): hung on its screen's user_data, made in build_chat, freed in on_delete (its timer
// first). The target is set once, in build_chat, so the compose line, Tab-retry and mark-read can only ever reach the
// chat this screen shows, whatever opens later. Two chat screens on the stack each keep their own.
typedef struct chat {
    struct chat       *next; // the live chat screens (chat_on_top)
    mesh_chat_target_t target;
    lv_obj_t          *screen, *list, *ta, *counter, *note;
    lv_obj_t          *tab_hop; // an empty second focusable: Tab moves the focus onto it, which retries the failed message
    lv_timer_t        *timer;
    mesh_msg_t        *copy;
    size_t             copy_n;
    uint32_t           shown_generation, shown_s;
    bool               note_error;
    bool               loaded; // on screen: mesh_notify knows it as the open chat (#50)

    // Search inside the chat (#48): the line that takes the compose line's place, and the hits among copy[]
    lv_obj_t *search_line;
    bool      searching;
    char      query[LIST_FILTER_MAX + 1];
    uint16_t  hits[SHOW_MESSAGES];
    size_t    hit_n, hit_pos;
    bool      have_hit;        // hit_seq names the current hit: it survives a rebuild by seq, not by index
    uint32_t  hit_seq;
    int32_t   search_from_y;   // where the list was when the search began: Esc goes back there
    scroll_anchor_t search_from; // ... on the same messages, which new ones move in the list (#127)
    bool      search_from_end; // ... or it was at the end: Esc goes to the end, messages may have come in since

    // Select mode (#111)
    bool     selecting;
    size_t   sel_pos;
    uint32_t sel_seq;
} chat_t;

static chat_t                   *chats;    // every chat screen that exists, newest first
static mesh_chat_target_t const *building; // the target build_chat commits; set only around its shell_open

static void add_line(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_wrap(parent, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
}

static void add_message(chat_t *c, mesh_msg_t const *m, uint32_t now, uint32_t now_epoch) {
    char age[24], status[32], meta[96];
    mesh_format_msg_age(m->loaded, m->heard_s, m->epoch, now, now_epoch, age, sizeof(age));
    mesh_status_kind_t kind = mesh_msg_status(m, status, sizeof(status));
    uint32_t color = kind == MESH_STATUS_FAILED ? THEME_RED : kind == MESH_STATUS_OK ? THEME_TEAL : THEME_DIM;
    if (m->outgoing) {
        snprintf(meta, sizeof(meta), "%s, %s", status, age);
    } else {
        snprintf(meta, sizeof(meta), "%s, %d dBm", age, m->rssi_x10 / 10);
    }
    lv_obj_t *item = lv_obj_create(c->list);
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
    lv_obj_set_user_data(item, (void *)(uintptr_t)m->seq); // its identity for the scroll anchor (rebuild)
    add_line(item, m->text, m->outgoing ? THEME_AMBER : THEME_TEXT);
    add_line(item, meta, color);
}

static void search_apply(chat_t *c, bool scroll);
static void select_apply(chat_t *c, bool scroll);
static void select_leave(chat_t *c);

// Scratch for shown_items, used within one call only (rebuild, search_enter/leave; all on the LVGL task)
EXT_RAM_BSS_ATTR static scroll_item_t anchor_items[SHOW_MESSAGES];

// The message rows on screen as scroll items, keyed by their seq: c->copy may hold the next snapshot already
static size_t shown_items(chat_t *c, scroll_item_t *items) {
    size_t n = lv_obj_get_child_count(c->list) >= c->copy_n ? c->copy_n : 0; // copy_n: the rows of the last rebuild
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *item  = lv_obj_get_child(c->list, (int32_t)i);
        items[i].top    = lv_obj_get_y(item); // in the content: does not move with scrolling
        items[i].bottom = items[i].top + lv_obj_get_height(item);
        items[i].key    = (uint32_t)(uintptr_t)lv_obj_get_user_data(item);
    }
    return n;
}

static void rebuild(chat_t *c, size_t n) {
    // At the end the view follows new messages. Above it, it stays on the messages being read (#127): by pixels alone
    // it slid a message on every new one, as the oldest of the window dropped off the top.
    scroll_item_t  *items  = anchor_items;
    bool            at_end = lv_obj_get_scroll_bottom(c->list) <= 0;
    int32_t         y      = lv_obj_get_scroll_y(c->list);
    scroll_anchor_t anchor = scroll_anchor_take(items, at_end ? 0 : shown_items(c, items), y);
    lv_obj_clean(c->list);
    c->copy_n = n;
    size_t shown = 0;
    uint32_t now = mesh_uptime_s(), now_epoch = mesh_epoch_now();
    for (size_t i = 0; i < n; i++) {
        add_message(c, &c->copy[i], now, now_epoch);
        shown++;
    }
    if (!shown) {
        add_line(c->list, c->target.dm ? "No messages yet: write the first one below." : "No messages yet: they appear when someone posts, or write the first one below.", THEME_DIM);
    }
    lv_obj_update_layout(c->list);
    if (!at_end) {
        scroll_anchor_find(&anchor, items, shown_items(c, items), &y); // y as it was when none of them is left
    }
    lv_obj_scroll_to_y(c->list, at_end ? LV_COORD_MAX : y, LV_ANIM_OFF);
    if (c->searching) {
        search_apply(c, false); // new rows: highlight them again, the view stays where it is
    }
    if (c->selecting) {
        select_apply(c, false);
    }
}

static size_t text_max(chat_t const *c) {
    return c->target.dm ? MF_MAX_TEXT : mesh_channel_text_max();
}

static void show_counter(chat_t *c) {
    char text[80];
    mesh_compose_status(lv_textarea_get_text(c->ta), text_max(c), text, sizeof(text));
    lv_label_set_text(c->counter, text);
}

static void refresh(chat_t *c) {
    uint32_t generation;
    // The chat's own ring (#59), read with mark_read while the chat is on screen: then it counts as read (#48). Not
    // under the screensaver or lock cover, nor with another screen on top such as the notification centre (#127):
    // nobody reads it there.
    bool   shown = c->screen == lv_screen_active();
    size_t n     = mesh_chat_snapshot(&c->target.key, c->copy, SHOW_MESSAGES, &generation, shown, SCREEN_WAIT_MS);
    if (n == MESH_BUSY) {
        return;
    }
    if (generation != c->shown_generation || mesh_uptime_s() - c->shown_s >= AGE_REFRESH_S) {
        rebuild(c, n);
        c->shown_generation = generation;
        c->shown_s          = mesh_uptime_s();
    }
}

static void tick(lv_timer_t *t) {
    refresh(lv_timer_get_user_data(t));
}

static void set_default_note(chat_t *c);

static void set_note(chat_t *c, char const *text, bool error) {
    lv_label_set_text(c->note, text);
    lv_obj_set_style_text_color(c->note, lv_color_hex(error ? THEME_RED : THEME_DIM), 0);
    c->note_error = error;
}

// The usual line under the composer; the clock hint takes its place while the clock is not set (#8) or only
// approximate (#83)
static void set_default_note(chat_t *c) {
    char const *clock = mesh_clock_hint(clock_is_set(), clock_is_approximate());
    set_note(c, clock ? clock : (c->target.dm ? HINT_DM : HINT_CHANNEL), false);
}

// Enter: the draft goes to this screen's own target, nothing else (#126)
static void on_ready(lv_event_t *e) {
    chat_t     *c    = lv_event_get_user_data(e);
    char const *text = lv_textarea_get_text(c->ta);
    if (!text[0]) {
        return;
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = c->target.dm ? mesh_send_dm(c->target.peer, text, MESH_FROM_SCREEN, &why)
                                     : mesh_send_channel(c->target.title, text, MESH_FROM_SCREEN, &why);
    if (!id) {
        set_note(c, why ? why : "not sent", true);
        return;
    }
    lv_textarea_set_text(c->ta, "");
    set_default_note(c);
    show_counter(c);
    c->shown_generation = UINT32_MAX; // the mesh task adds it within a few ms: the next tick shows it
}

static void on_changed(lv_event_t *e) {
    chat_t *c = lv_event_get_user_data(e);
    show_counter(c);
    if (c->note_error) {
        set_default_note(c);
    }
}

// Tab: start over with the newest failed DM of this chat
static void retry_failed(chat_t *c) {
    for (size_t i = c->copy_n; i > 0; i--) {
        mesh_msg_t const *m = &c->copy[i - 1];
        if (m->outgoing && mesh_msg_in_dm(m, c->target.peer) && m->state == MESH_OUT_FAILED) {
            if (mesh_retry(m->id)) {
                set_note(c, "Trying again", false);
                c->shown_generation = UINT32_MAX;
            } else {
                set_note(c, "Cannot retry now", true);
            }
            return;
        }
    }
    set_note(c, "Nothing failed to retry", false);
}

// ---- search inside the chat (#48) -----------------------------------------------------------------------------------

// Highlights the hits (THEME_TILE) and frames the current one (amber left border); scroll brings the current one into
// view. Works on copy[], the snapshot the screen already holds: no second mesh lock.
static void search_apply(chat_t *c, bool scroll) {
    // copy[] may be a new snapshot already (rebuild): resolve the current hit by its seq, never by an old index
    c->hit_n = mesh_chat_rehit(c->copy, c->copy_n, c->query, c->hits, SHOW_MESSAGES, c->have_hit, c->hit_seq,
                               &c->hit_pos);
    c->have_hit = c->hit_pos < c->hit_n;
    c->hit_seq  = c->have_hit ? c->copy[c->hits[c->hit_pos]].seq : 0;
    size_t h = 0;
    for (size_t i = 0; i < c->copy_n; i++) {
        lv_obj_t *item = lv_obj_get_child(c->list, (int32_t)i);
        bool      hit  = h < c->hit_n && c->hits[h] == i;
        if (!item) {
            break;
        }
        lv_obj_set_style_bg_opa(item, hit ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(item, hit && h == c->hit_pos ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        h += hit;
    }
    char text[96];
    mesh_chat_search_line(c->query, c->hit_pos, c->hit_n, text, sizeof(text));
    lv_label_set_text(c->search_line, text);
    if (scroll && c->hit_pos < c->hit_n) {
        lv_obj_t *item = lv_obj_get_child(c->list, c->hits[c->hit_pos]);
        if (item) {
            lv_obj_update_layout(c->list);
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        }
    }
}

static void search_enter(chat_t *c) {
    lv_group_t *group = lv_obj_get_group(c->ta);
    if (!group || !c->search_line || c->searching) {
        return;
    }
    c->searching       = true;
    c->query[0]        = '\0';
    c->hit_n           = 0;
    c->hit_pos         = MESH_CHAT_NO_HIT;
    c->have_hit        = false;
    c->search_from_y   = lv_obj_get_scroll_y(c->list);
    c->search_from_end = lv_obj_get_scroll_bottom(c->list) <= 0;
    c->search_from     = scroll_anchor_take(anchor_items, c->search_from_end ? 0 : shown_items(c, anchor_items),
                                            c->search_from_y);
    // The search line takes the focus (and the keys) before the compose line hides: hiding a focused object moves the
    // focus on, which in a DM is the Tab-retry hop
    lv_group_add_obj(group, c->search_line);
    lv_obj_set_hidden(c->search_line, false);
    lv_group_focus_obj(c->search_line);
    lv_obj_set_hidden(c->ta, true);
    lv_obj_set_hidden(c->counter, true);
    set_note(c, HINT_SEARCH, false);
    search_apply(c, false);
}

// Enter keeps the view where the search took it; Esc (restore) goes back to where the search began
static void search_leave(chat_t *c, bool restore) {
    if (!c->searching) {
        return;
    }
    c->searching = false;
    c->query[0]  = '\0';
    search_apply(c, false); // no query, no hits: every row plain again
    lv_obj_set_hidden(c->ta, false);
    lv_obj_set_hidden(c->counter, false);
    lv_group_focus_obj(c->ta);
    lv_group_remove_obj(c->search_line);
    lv_obj_set_hidden(c->search_line, true);
    set_default_note(c);
    if (restore) {
        // By the messages that were in view, not the pixels: new ones may have pushed them along since (#127)
        lv_obj_update_layout(c->list);
        int32_t y = c->search_from_y; // as it was when none of them is left
        if (!c->search_from_end) {
            scroll_anchor_find(&c->search_from, anchor_items, shown_items(c, anchor_items), &y);
        }
        lv_obj_scroll_to_y(c->list, c->search_from_end ? LV_COORD_MAX : y, LV_ANIM_OFF);
    }
}

// ---- select mode (#111) ---------------------------------------------------------------------------------------------

// Frames message sel_pos like search's current hit. The list was rebuilt maybe: the message is found again by its seq,
// and select mode ends when it scrolled out of the window.
static void select_apply(chat_t *c, bool scroll) {
    if (c->selecting) {
        c->sel_pos = mesh_chat_select_of_seq(c->copy, c->copy_n, c->sel_seq);
        if (c->sel_pos == MESH_CHAT_NO_HIT) {
            select_leave(c);
            return;
        }
    }
    for (size_t i = 0; i < c->copy_n; i++) {
        lv_obj_t *item = lv_obj_get_child(c->list, (int32_t)i);
        if (!item) {
            break;
        }
        bool on = c->selecting && i == c->sel_pos;
        lv_obj_set_style_bg_opa(item, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(item, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
    if (scroll && c->selecting) {
        lv_obj_t *item = lv_obj_get_child(c->list, (int32_t)c->sel_pos);
        if (item) {
            lv_obj_update_layout(c->list);
            lv_obj_scroll_to_view(item, LV_ANIM_OFF);
        }
    }
}

static void select_enter(chat_t *c) {
    size_t pos = mesh_chat_select_step(MESH_CHAT_NO_HIT, c->copy_n, -1);
    if (c->selecting || c->searching || pos == MESH_CHAT_NO_HIT) {
        return;
    }
    c->selecting = true;
    c->sel_pos   = pos;
    c->sel_seq   = c->copy[pos].seq;
    set_note(c, HINT_SELECT, false);
    select_apply(c, true);
}

// Back to typing; the compose line kept the focus all along
static void select_leave(chat_t *c) {
    if (!c->selecting) {
        return;
    }
    c->selecting = false;
    c->sel_pos   = MESH_CHAT_NO_HIT;
    select_apply(c, false);
    set_default_note(c);
}

// Enter on someone else's channel message starts the compose line with their mention; anything else just goes back
static void select_pick(chat_t *c) {
    char mention[MESH_NAME_MAX * 2 + 8];
    bool can = !c->target.dm && c->sel_pos < c->copy_n && mesh_chat_mention(&c->copy[c->sel_pos], mention, sizeof(mention));
    bool own = c->sel_pos < c->copy_n && c->copy[c->sel_pos].outgoing;
    select_leave(c);
    if (!can) {
        if (!c->target.dm && !own) {
            set_note(c, "Cannot mention that name", true);
        }
        return;
    }
    char const *draft = lv_textarea_get_text(c->ta);
    if (!mesh_chat_mention_fits(draft, mention, text_max(c))) {
        set_note(c, "Too long to mention", true);
        return;
    }
    lv_textarea_set_cursor_pos(c->ta, 0);
    lv_textarea_add_text(c->ta, mention);
    lv_textarea_set_cursor_pos(c->ta, LV_TEXTAREA_CURSOR_LAST);
}

// Keys while selecting: Up/Down step, Enter picks, everything else is swallowed (Esc is the shell's: on_esc)
static void select_key(chat_t *c, uint32_t key) {
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        size_t pos = mesh_chat_select_step(c->sel_pos, c->copy_n, key == LV_KEY_UP ? -1 : 1);
        if (pos == MESH_CHAT_NO_HIT) {
            select_leave(c);
            return;
        }
        c->sel_pos = pos;
        c->sel_seq = c->copy[pos].seq;
        select_apply(c, true);
    } else if (key == LV_KEY_ENTER) {
        select_pick(c);
    }
}

// The compose line sees a key before the textarea does: "/" on an empty line starts a search and never reaches the
// draft
static void on_compose_key(lv_event_t *e) {
    chat_t  *c   = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (c->selecting) {
        lv_event_stop_processing(e);
        select_key(c, key);
    } else if (key == '/' && !lv_textarea_get_text(c->ta)[0]) {
        lv_event_stop_processing(e);
        search_enter(c);
    } else if (key == LV_KEY_UP && !lv_textarea_get_text(c->ta)[0] && c->copy_n) {
        lv_event_stop_processing(e); // with a draft Up keeps scrolling the list (on_key)
        select_enter(c);
    }
}

// Keys while searching (the search line has the focus): typing and Backspace edit the query, Up/Down jump between
// hits, Enter is done
static void on_search_key(lv_event_t *e) {
    chat_t  *c   = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (!c->searching) {
        return;
    }
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (c->hit_n == 0) {
            return;
        }
        c->hit_pos  = mesh_chat_hit_step(c->hit_pos, c->hit_n, key == LV_KEY_UP ? -1 : 1);
        c->hit_seq  = c->copy[c->hits[c->hit_pos]].seq; // hits[] matches copy[]: both were set by the last search_apply
        c->have_hit = true;
        search_apply(c, true);
    } else if (key == LV_KEY_ENTER) {
        search_leave(c, false);
    } else if (list_filter_edit(c->query, sizeof(c->query), key)) {
        c->have_hit = false; // a new query starts at its newest hit
        search_apply(c, true);
    }
}

// The group is this screen's own (shell_open makes one per screen): its user_data is the screen's chat
static void on_focus_move(lv_group_t *group) {
    chat_t *c = lv_group_get_user_data(group);
    if (!c) {
        return;
    }
    if (c->searching && lv_group_get_focused(group) != c->search_line) {
        lv_group_focus_obj(c->search_line); // Tab does nothing while searching (no retry from a hidden compose line)
        return;
    }
    if (c->tab_hop && lv_group_get_focused(group) == c->tab_hop) {
        if (c->selecting) {
            lv_group_focus_obj(c->ta); // no retry while a message is framed
            return;
        }
        retry_failed(c);
        lv_group_focus_obj(c->ta); // calls this again, with the compose line focused: a no-op
    }
}

static void on_key(lv_event_t *e) {
    chat_t *c = lv_event_get_user_data(e);
    switch (lv_event_get_key(e)) {
        case LV_KEY_UP: shell_scroll_by(c->list, SCROLL_STEP_PX); break;
        case LV_KEY_DOWN: shell_scroll_by(c->list, -SCROLL_STEP_PX); break;
        default: break;
    }
}

// Esc leaves select mode or a search first (#48), then clears a draft (#57), then leaves
static bool on_esc(void *arg) {
    chat_t *c = arg;
    if (c->selecting) {
        select_leave(c);
        return true;
    }
    if (c->searching) {
        search_leave(c, true);
        return true;
    }
    if (!c->ta || !lv_textarea_get_text(c->ta)[0]) {
        return false;
    }
    lv_textarea_set_text(c->ta, "");
    return true;
}

// The chat is on screen (#50): no notice is posted for it and its notices are marked read
static void on_loaded(lv_event_t *e) {
    chat_t *c = lv_event_get_user_data(e);
    c->loaded = true;
    mesh_notify_set_open_chat(&c->target.key);
}

static void on_unload(lv_event_t *e) {
    chat_t *c = lv_event_get_user_data(e);
    if (c->loaded) {
        mesh_notify_clear_open_chat(&c->target.key); // only if it is still the open one
        c->loaded = false;
    }
}

static void chat_unlink(chat_t *c) {
    for (chat_t **p = &chats; *p; p = &(*p)->next) {
        if (*p == c) {
            *p = c->next;
            return;
        }
    }
}

// The screen goes (its children after this): the timer first, so tick never runs on freed state, then the state
static void on_delete(lv_event_t *e) {
    chat_t *c = lv_event_get_user_data(e);
    on_unload(e);
    if (c->timer) {
        lv_timer_delete(c->timer);
        c->timer = NULL;
    }
    // The focus callback was set on this screen's group; the shell usually deletes the group first (then ta has none)
    lv_group_t *group = c->ta ? lv_obj_get_group(c->ta) : NULL;
    if (group) {
        lv_group_set_focus_cb(group, NULL);
        lv_group_set_user_data(group, NULL);
    }
    lv_obj_set_user_data(c->screen, NULL);
    chat_unlink(c);
    lichen_free(meshcore_ctx(), c->copy);
    lichen_free(meshcore_ctx(), c);
}

static void style_ta(lv_obj_t *ta) {
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
    chat_t *c = arg;
    if (!c->selecting) {
        search_enter(c); // does nothing while a search is open
    }
}

static lv_obj_t *create_search_line(chat_t *c) {
    // Takes the compose line's place while searching (#48), styled like it so nothing else moves
    lv_obj_t *line = lichen_label_line(c->screen, LV_PCT(100));
    lv_obj_set_style_bg_color(line, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(line, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(line, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_radius(line, 14, 0);
    lv_obj_set_style_pad_all(line, 14, 0);
    lv_obj_set_style_border_width(line, 3, 0);
    lichen_label_line_fit(line); // one line of the font plus the padding and border, so DOT cuts instead of wrapping
    lv_obj_set_style_border_color(line, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_outline_width(line, 0, LV_STATE_ANY);
    lv_obj_set_hidden(line, true);
    lv_obj_add_event_cb(line, on_search_key, LV_EVENT_KEY, c);
    return line;
}

static void create_widgets(chat_t *c) {
    lv_obj_t *screen = c->screen;
    lv_obj_t *heading = lichen_label_line(screen, LV_PCT(100)); // DOT without a height wraps a long name (#132)
    lv_obj_set_style_text_font(heading, lichen_font(LICHEN_FONT_LARGE), 0);
    lichen_label_line_fit(heading);
    lv_obj_set_style_text_color(heading, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(heading, c->target.title); // "@bob" for a DM, as the Chat list's rows (#101)

    c->list = lv_obj_create(screen);
    lv_obj_remove_style_all(c->list);
    lv_obj_set_width(c->list, LV_PCT(100));
    lv_obj_set_flex_grow(c->list, 1);
    lv_obj_set_flex_flow(c->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c->list, 8, 0);
    lv_obj_set_scroll_dir(c->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c->list, LV_SCROLLBAR_MODE_ACTIVE);
    shell_scroll_tame(c->list);

    c->ta = lv_textarea_create(screen);
    lv_textarea_set_one_line(c->ta, true);
    lv_textarea_set_max_length(c->ta, (uint32_t)text_max(c));
    lv_textarea_set_placeholder_text(c->ta, "Write a message");
    style_ta(c->ta);
    lv_obj_add_event_cb(c->ta, on_ready, LV_EVENT_READY, c);
    lv_obj_add_event_cb(c->ta, on_changed, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_add_event_cb(c->ta, on_key, LV_EVENT_KEY, c);
    lv_obj_add_event_cb(c->ta, on_compose_key, LV_EVENT_KEY | LV_EVENT_PREPROCESS, c);

    c->search_line = create_search_line(c);

    c->counter = lv_label_create(screen);
    lv_obj_set_width(c->counter, LV_PCT(100));
    c->note = lv_label_create(screen);
    lv_obj_set_width(c->note, LV_PCT(100));
    set_default_note(c);
    lv_obj_set_style_text_color(c->counter, lv_color_hex(THEME_DIM), 0);
    show_counter(c);
}

// Runs inside shell_open, so only once the shell pushed the screen: the target is committed here and nowhere else
static lv_obj_t *build_chat(void) {
    if (!building) {
        return NULL;
    }
    chat_t *c = lichen_alloc(meshcore_ctx(), sizeof(*c), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    mesh_msg_t *copy = c ? lichen_alloc(meshcore_ctx(), SHOW_MESSAGES * sizeof(*copy), LICHEN_MEM_BULK) : NULL;
    if (!copy) {
        ESP_LOGW("meshcore_chat", "no memory for a chat screen");
        lichen_free(meshcore_ctx(), c);
        return NULL; // shell_open takes the screen off the stack again
    }
    c->target           = *building;
    c->copy             = copy;
    c->sel_pos          = MESH_CHAT_NO_HIT;
    c->hit_pos          = MESH_CHAT_NO_HIT;
    c->shown_generation = UINT32_MAX;

    lv_obj_t *screen = shell_page_create();
    c->screen        = screen;
    lv_obj_set_user_data(screen, c);
    lv_obj_set_style_pad_bottom(screen, THEME_PAD, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 10, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, c);
    lv_obj_add_event_cb(screen, on_loaded, LV_EVENT_SCREEN_LOADED, c);
    lv_obj_add_event_cb(screen, on_unload, LV_EVENT_SCREEN_UNLOAD_START, c);
    shell_on_esc(on_esc, c);
    shell_on_search(on_search, c);
    c->next = chats;
    chats   = c;

    create_widgets(c);

    lv_group_t *group = lv_group_get_default(); // the screen's own, made by shell_open
    if (c->target.dm && group) {
        c->tab_hop = lv_obj_create(screen);
        lv_obj_remove_style_all(c->tab_hop);
        lv_obj_set_size(c->tab_hop, 0, 0);
        lv_obj_set_floating(c->tab_hop, true);
        lv_obj_set_clickable(c->tab_hop, false);
        lv_group_add_obj(group, c->tab_hop);
        lv_group_focus_obj(c->ta);
    }
    if (group) {
        lv_group_set_user_data(group, c);
        lv_group_set_focus_cb(group, on_focus_move); // keeps the focus on a search (#48), Tab retries (#47)
    }
    c->timer = lv_timer_create(tick, TICK_MS, c); // NULL without memory: the chat then shows what refresh found once
    refresh(c);
    return screen;
}

// ---- opening a chat (#126) --------------------------------------------------------------------------------------------

// The chat screen on top of the shell's stack, NULL when the top screen is not a chat. The stack, not lv_screen_active:
// under a screensaver or lock cover the chat is still the one a new chat would land on.
static chat_t *chat_on_top(void) {
    lv_obj_t *top = shell_stack_screen(0);
    for (chat_t *c = chats; top && c; c = c->next) {
        if (c->screen == top) {
            return c;
        }
    }
    return NULL;
}

typedef struct {
    mesh_chat_target_t want;
    lv_obj_t          *below; // the screen under the chat being replaced; compared only, it may be gone
} replace_t;

static void open_target(mesh_chat_target_t const *want);

// shell_back_to's follow-up: when the screen below the old chat is on top now, open (a queued Esc may have popped the
// chat already: back_to then pops nothing). Anything else (the pop refused by the update screen, more popped by
// further Escs): open nothing.
static void open_after_pop(void *arg) {
    replace_t *r = arg;
    if (shell_stack_screen(0) == r->below) {
        open_target(&r->want);
    }
    lichen_free(meshcore_ctx(), r);
}

// LVGL task. The same chat on top: nothing happens, the draft stays. Another chat on top: back to the screen below it
// (Esc from the new one then goes where Esc from the old one went), then the new one opens. Otherwise a new chat screen
// is pushed.
static void open_target(mesh_chat_target_t const *want) {
    chat_t *top = chat_on_top();
    switch (mesh_chat_open_how(top ? &top->target : NULL, want)) {
        case MESH_CHAT_OPEN_KEEP: shell_open_count_reuse(); return; // shown: the notice counts as opened
        case MESH_CHAT_OPEN_REPLACE: {
            lv_obj_t  *below = shell_stack_screen(1);
            replace_t *r     = below ? lichen_alloc(meshcore_ctx(), sizeof(*r), LICHEN_MEM_BULK) : NULL;
            if (!r) {
                return;
            }
            r->want  = *want;
            r->below = below;
            if (!shell_back_to(below, open_after_pop, r)) {
                lichen_free(meshcore_ctx(), r); // not queued: open_after_pop will never run
            } else {
                shell_open_count_reuse(); // the chat is shown in a moment
            }
            return;
        }
        case MESH_CHAT_OPEN_PUSH:
            building = want;
            shell_open_keyed(build_chat, want->dm ? SCREEN_DM : SCREEN_CHAT, shell_key_hash(&want->key, sizeof(want->key)));
            building = NULL;
            return;
    }
}

void meshcore_chat_open_channel(char const *name) {
    mesh_chat_target_t want; // name may be a row of a list that is rebuilt while the chat opens: copied here
    mesh_chat_target_channel(&want, name);
    open_target(&want);
}

void meshcore_chat_open_dm(uint8_t const prefix[6], char const *title) {
    mesh_chat_target_t want;
    mesh_chat_target_dm(&want, prefix, title);
    open_target(&want);
}

// ---- the contact picker, then the DM chat (#47) --------------------------------------------------------------------

static mesh_contact_t *peers;

static void open_dm(void *arg) {
    mesh_contact_t const *c = arg;
    meshcore_chat_open_dm(c->pubkey, c->name);
}

static void free_peers(lv_event_t *e) {
    (void)e;
    lichen_free(meshcore_ctx(), peers);
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
    lichen_free(meshcore_ctx(), peers);
    peers = lichen_alloc(meshcore_ctx(), MAX_PEERS * sizeof(*peers), LICHEN_MEM_BULK);
    size_t total, n = peers ? mesh_contacts_snapshot(peers, MAX_PEERS, &total, SCREEN_WAIT_MS) : 0;
    if (!peers || n == MESH_BUSY) {
        menu_note(menu, mc_busy_note());
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
        menu_empty(menu, "contacts to message", "chat nodes appear when they advert");
    }
    return screen;
}

void meshcore_chat_pick_contact(void) {
    shell_open(build_dm_pick, SCREEN_DM_PICK);
}
