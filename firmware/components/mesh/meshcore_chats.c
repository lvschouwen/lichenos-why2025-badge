// The Chat tile's screen (#88): "New message..." (the contact picker, for a DM nobody has started yet) and then the
// conversation list with unread badges (#48), from the old Messages tab. Rebuilt when
// a message arrives, a count changes or the channels do, and every AGE_REFRESH_S so "N min ago" stays right. Copies
// are taken from the mesh (PSRAM) under its lock, the widgets are built from them afterwards.
#include "esp_heap_caps.h"
#include "screen_ids.h"
#include "esp_log.h"
#include "lichen_ui.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_chat_target.h"
#include "meshcore_chat.h"
#include "meshcore_list.h"
#include "meshcore_screens.h"
#include "theme.h"

#include <stdint.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define SHOW_CONVS     MESH_CONV_MAX // one row per chat
#define SHOW_ROWS      (SHOW_CONVS + 1) // and "New message..." on top
#define NEW_ROW_KEY    0x4E45574Du // "NEWM": the scroll anchor's key of that row
#define MAX_CHANNELS   40
#define AGE_REFRESH_S  30
#define SCREEN_WAIT_MS 20 // for the mesh lock; LVGL (and with it the keys) must not wait on MeshCore's file saves

// One Chat list's state (#147): made in build, hung on its screen's user_data, freed in on_delete (its timer first). A
// second Chat list on the stack therefore cannot touch the first one's. One PSRAM block.
typedef struct chats {
    struct chats   *next; // the live Chat lists (meshcore_chats_open)
    lv_obj_t       *screen;
    lv_timer_t     *timer;
    mc_list_t       list;
    uint32_t        shown_sig, last_rebuild_s;
    mesh_chat_key_t sel_key; // the highlight follows its chat when the order changes
    bool            sel_valid;
    bool            sel_new; // the highlight is on "New message..." (row 0; chat i is row i + 1)
    mesh_conv_t     convs[SHOW_CONVS];
    char            chan_names[MAX_CHANNELS][MESH_CHANNEL_MAX + 1];
    uint32_t        keys[SHOW_ROWS];
    scroll_item_t   items[SHOW_ROWS];
} chats_t;

static chats_t *live; // every Chat list that exists, newest first

// One chat: title and unread badge on top, the age and latest line under them (mesh_conv_row)
static void add_conv_row(chats_t *st, mesh_conv_t const *c, uint32_t now, uint32_t now_epoch, bool selected) {
    char title[MESH_CHANNEL_MAX + 1], badge[8], sub[MESH_TEXT_MAX + 40];
    mesh_conv_row(c, now, now_epoch, title, sizeof(title), badge, sizeof(badge), sub, sizeof(sub));
    lv_obj_t *row = mc_list_row(&st->list, selected);
    lv_obj_t *top = lv_obj_create(row);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_set_scrollable(top, false);
    lv_obj_set_clickable(top, false);
    lv_obj_t *t = mc_list_line(top, title, THEME_TEXT);
    lv_obj_set_flex_grow(t, 1);
    if (badge[0]) {
        mc_list_line(top, badge, THEME_AMBER);
    }
    lv_obj_t *s = mc_list_line(row, sub, THEME_DIM);
    lv_obj_set_width(s, LV_PCT(100));
}

// Where the highlight is after the st->list changed: on "New message..." or the same chat if it is still there, else the
// same row
static void settle_selection(chats_t *st, size_t count) {
    size_t at = SIZE_MAX;
    for (size_t i = 0; !st->sel_new && st->sel_valid && i < count; i++) {
        if (memcmp(&st->convs[i].key, &st->sel_key, sizeof(st->sel_key)) == 0) {
            at = i + 1;
            break;
        }
    }
    st->list.sel  = st->sel_new ? 0 : at != SIZE_MAX ? at : (st->list.sel <= count ? st->list.sel : count);
    st->sel_new   = st->list.sel == 0;
    st->sel_valid = !st->sel_new;
    if (st->sel_valid) {
        st->sel_key = st->convs[st->list.sel - 1].key;
    }
}

static void add_new_row(chats_t *st, bool selected) {
    lv_obj_t *row = mc_list_row(&st->list, selected);
    mc_list_line(row, "New message...", THEME_TEAL);
    mc_list_line(row, "to a contact, type to search", THEME_DIM);
}

static void rebuild(chats_t *st, size_t count) {
    mc_scroll_t s = mc_list_save(&st->list);
    lv_obj_clean(st->list.list);
    st->list.n = count + 1;
    settle_selection(st, count);
    st->list.keys[0] = NEW_ROW_KEY;
    add_new_row(st, st->list.sel == 0);
    if (!count) {
        mc_list_label(st->list.list, mesh_running() ? "No chats yet." : "MeshCore is not running.", THEME_DIM);
        return; // one row: nothing to move to, so the extra label does not matter
    }
    uint32_t now = mesh_uptime_s(), now_epoch = mesh_epoch_now();
    for (size_t i = 0; i < count; i++) {
        st->list.keys[i + 1] = scroll_key_hash(&st->convs[i].key, sizeof(st->convs[i].key), 0);
        add_conv_row(st, &st->convs[i], now, now_epoch, i + 1 == st->list.sel);
    }
    mc_list_restore(&st->list, s);
}

// The chats' signature: changes when a message arrives, a count changes or the channels do
static uint32_t convs_sig(chats_t const *st, size_t n) {
    uint32_t h = scroll_key_hash(&n, sizeof(n), 0);
    for (size_t i = 0; i < n; i++) {
        h = scroll_key_hash(&st->convs[i].key, sizeof(st->convs[i].key), h);
        h = scroll_key_hash(&st->convs[i].last_seq, sizeof(st->convs[i].last_seq), h);
        h = scroll_key_hash(&st->convs[i].unread, sizeof(st->convs[i].unread), h);
        h = scroll_key_hash(&st->convs[i].contact_gone, sizeof(st->convs[i].contact_gone), h);
    }
    return h;
}

// The store's chats plus the configured channels, newest first. The copy is only complete once both snapshots are in,
// so the rows on screen and the copy Enter opens from always agree.
static size_t take_convs(chats_t *st) {
    size_t names = mesh_channels_snapshot_wait(st->chan_names, MAX_CHANNELS, SCREEN_WAIT_MS);
    if (names == MESH_BUSY) {
        return MESH_BUSY;
    }
    size_t n = mesh_convs_snapshot(st->convs, SHOW_CONVS, SCREEN_WAIT_MS);
    if (n == MESH_BUSY) {
        return MESH_BUSY;
    }
    n = mesh_conv_merge_channels(st->convs, n, SHOW_CONVS, (char const(*)[MESH_CHANNEL_MAX + 1])st->chan_names,
                                 names);
    mesh_conv_sort(st->convs, n);
    return n;
}

static void refresh(chats_t *st, bool from_timer) {
    if (from_timer && lv_screen_active() != st->screen) {
        return; // a chat is open on top: no mesh lock for rows nobody sees (they catch up on the next tick back)
    }
    size_t count = take_convs(st);
    if (count == MESH_BUSY) {
        return; // MeshCore is busy (saving files): the screen stays as it is until the next tick
    }
    uint32_t sig   = convs_sig(st, count);
    bool     stale = mesh_uptime_s() - st->last_rebuild_s >= AGE_REFRESH_S;
    if (sig != st->shown_sig || stale) {
        rebuild(st, count);
        st->shown_sig      = sig;
        st->last_rebuild_s = mesh_uptime_s();
    }
}

static void open_selected(chats_t *st) {
    if (st->list.sel == 0) {
        meshcore_chat_pick_contact(); // a DM with a companion that has no conversation yet
        return;
    }
    if (!st->sel_valid || st->list.sel >= st->list.n) {
        return;
    }
    mesh_conv_t const *c = &st->convs[st->list.sel - 1];
    if (c->key.kind == MESH_CHAT_DM) {
        meshcore_chat_open_dm(c->key.id, c->title);
    } else {
        meshcore_chat_open_channel(c->title);
    }
}

static void tick(lv_timer_t *t) {
    refresh(lv_timer_get_user_data(t), true);
}

static void on_key(lv_event_t *e) {
    chats_t *st  = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (mc_list_move(&st->list, key == LV_KEY_UP ? -1 : 1)) {
            st->sel_new   = st->list.sel == 0;
            st->sel_valid = !st->sel_new;
            if (st->sel_valid) {
                st->sel_key = st->convs[st->list.sel - 1].key;
            }
        }
    } else if (key == LV_KEY_ENTER) {
        open_selected(st);
    }
}

static void on_delete(lv_event_t *e) {
    chats_t *st = lv_event_get_user_data(e);
    for (chats_t **p = &live; *p; p = &(*p)->next) {
        if (*p == st) {
            *p = st->next;
            break;
        }
    }
    if (st->timer) {
        lv_timer_delete(st->timer);
    }
    heap_caps_free(st);
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    chats_t  *st     = heap_caps_calloc(1, sizeof(*st), MALLOC_CAP_SPIRAM);
    if (!st) {
        ESP_LOGE(TAG, "no memory for the chats");
        mc_list_label(screen, "Not enough memory to show the chats.", THEME_DIM);
        return screen;
    }
    st->screen = screen;
    st->next   = live;
    live       = st;
    lv_obj_set_user_data(screen, st);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, st);
    st->list      = (mc_list_t){.keys = st->keys, .items = st->items, .cap = SHOW_ROWS, .sel = 1};
    st->list.list = mc_list_screen(screen, "Chat", NULL, "Enter", "open");
    lv_obj_add_event_cb(st->list.list, on_key, LV_EVENT_KEY, st);
    st->shown_sig      = UINT32_MAX;
    st->sel_new        = false; // the newest chat is highlighted first, as before; Up reaches "New message..."
    st->last_rebuild_s = mesh_uptime_s();
    st->timer          = lv_timer_create(tick, 1000, st);
    refresh(st, false); // not loaded yet: the rows right away all the same
    return screen;
}

void meshcore_chats_open(void) {
    shell_open(build, SCREEN_CHATS);
}
