// MeshCore as a Lichen app (#67): its home tile (unread, nodes, the latest message), its screen with two tabs, Messages
// and Contacts (from the old ui/ui.c), its about section under Settings and its background part (the mesh task, started
// from main.c whatever screen is open). Keys on the screen: Left/Right or Tab switch tabs, Up/Down scroll, Esc is the
// shell's. Everything with an lv_obj_t runs in the LVGL task; it reads MeshCore only through mesh.h copies.
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lichen_app.h"
#include "lora.h"
#include "mc_first.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "meshcore_settings.h"
#include "scroll_anchor.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "shell.h"
#include "theme.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define SHOW_MESSAGES  50 // newest kept on screen
#define SHOW_CONTACTS  100
#define AGE_REFRESH_S  30 // rebuild the lists this often even when nothing new arrived, so "N min ago" stays right
#define SCROLL_STEP_PX 120
#define SCREEN_WAIT_MS 20 // for the mesh lock; LVGL (and with it the keys) must not wait on MeshCore's file saves
#define TAB_BAR_H      64
#define TAB_MESSAGES   0
#define KEY_W          150

// ---- the icon (home.html <g id="mc">: two arcs over a dot, in a 60x60 view) ---------------------------------------

static void draw_icon(lv_event_t *e) {
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   box;
    lv_obj_get_coords(obj, &box);
    int32_t size = lv_area_get_width(&box);
    int32_t cx   = box.x1 + size / 2;
    int32_t cy   = box.y1 + size * 42 / 60;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color       = lv_color_hex(THEME_AMBER);
    arc.width       = LV_MAX(2, size * 5 / 60);
    arc.rounded     = 1;
    arc.center.x    = cx;
    arc.center.y    = cy;
    arc.start_angle = 205;
    arc.end_angle   = 335;
    arc.radius      = (uint16_t)(size * 22 / 60);
    lv_draw_arc(layer, &arc);
    arc.radius = (uint16_t)(size * 13 / 60);
    arc.opa    = LV_OPA_60;
    lv_draw_arc(layer, &arc);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius   = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(THEME_AMBER);
    int32_t   r  = LV_MAX(2, size * 5 / 60);
    lv_area_t a  = {cx - r, cy - r, cx + r - 1, cy + r - 1};
    lv_draw_rect(layer, &dot, &a);
}

static void icon(lv_obj_t *parent, int size_px) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size_px, size_px);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    lv_obj_center(obj);
    lv_obj_add_event_cb(obj, draw_icon, LV_EVENT_DRAW_MAIN, NULL);
}

// ---- the tile ----------------------------------------------------------------------------------------------------

static bool tile_text(char *l1, size_t c1, char *l2, size_t c2) {
    if (!mesh_running()) {
        snprintf(l1, c1, "Not running");
        snprintf(l2, c2, "%s", lora_up() ? "" : "Needs the LoRa radio");
        return true;
    }
    mesh_tile_t t;
    mesh_tile_snapshot(&t);
    mesh_tile_text(&t, l1, c1, l2, c2);
    return true;
}

// ---- the screen: Messages and Contacts ---------------------------------------------------------------------------

static lv_obj_t       *tabs;
static lv_obj_t       *messages_list;
static lv_obj_t       *contacts_list;
static lv_obj_t       *tab_hop; // an empty second focusable: Tab moves the focus onto it, which switches the tab
static lv_timer_t     *timer;
static uint32_t        shown_generation;
static uint32_t        shown_contacts_sig;
static uint32_t        last_rebuild_s;
// Copies taken from the mesh (PSRAM): the widgets are built from these, outside the mesh lock
static mesh_msg_t     *msg_copy;
static mesh_contact_t *contact_copy;

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_item(lv_obj_t *list) {
    lv_obj_t *item = lv_obj_create(list);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_ver(item, 4, 0);
    lv_obj_set_style_pad_row(item, 2, 0);
    lv_obj_set_scrollable(item, false);
    lv_obj_set_clickable(item, false);
    return item;
}

// Rebuilding a list keeps where the reader was: at the newest end if they were there, else on the same item (by
// identity, scroll_anchor.c): a pixel offset jumped one item when the full message window dropped its oldest or the
// contacts changed order. keys[i] is the identity of the list's child i while the list shows items.
// In PSRAM with the copies (internal RAM is the display's), allocated while the screen exists; LVGL task only.
typedef struct {
    uint32_t      msg[SHOW_MESSAGES];
    uint32_t      contact[SHOW_CONTACTS];
    scroll_item_t items[SHOW_CONTACTS > SHOW_MESSAGES ? SHOW_CONTACTS : SHOW_MESSAGES];
} anchor_mem_t;
static anchor_mem_t  *anchor_mem;
static uint32_t      *msg_keys, *contact_keys;
static scroll_item_t *scroll_items;
static size_t         msg_keys_n, contact_keys_n;

typedef struct {
    int32_t         y;
    bool            at_end;
    scroll_anchor_t anchor;
} scroll_t;

// The list's items with their keys; 0 when the children are not the keyed items (the "nothing yet" line)
static size_t list_items(lv_obj_t *list, uint32_t const *keys, size_t n) {
    if (n == 0 || lv_obj_get_child_count(list) != n) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        lv_obj_t *item         = lv_obj_get_child(list, (int32_t)i);
        scroll_items[i].top    = lv_obj_get_y(item); // in the content: does not move with scrolling
        scroll_items[i].bottom = scroll_items[i].top + lv_obj_get_height(item);
        scroll_items[i].key    = keys[i];
    }
    return n;
}

static scroll_t save_scroll(lv_obj_t *list, bool newest_at_bottom, uint32_t const *keys, size_t n) {
    scroll_t s = {.y = lv_obj_get_scroll_y(list)};
    s.at_end   = newest_at_bottom ? lv_obj_get_scroll_bottom(list) <= 0 : s.y <= 0;
    s.anchor   = scroll_anchor_take(scroll_items, list_items(list, keys, n), s.y);
    return s;
}

static void restore_scroll(lv_obj_t *list, scroll_t s, bool newest_at_bottom, uint32_t const *keys, size_t n) {
    lv_obj_update_layout(list);
    int32_t y = s.y;
    if (s.at_end) {
        y = newest_at_bottom ? LV_COORD_MAX : 0;
    } else {
        scroll_anchor_find(&s.anchor, scroll_items, list_items(list, keys, n), &y);
    }
    // scroll_to_y is bounded: a list that fits doesn't move, a shorter one doesn't end up overscrolled
    lv_obj_scroll_to_y(list, y, LV_ANIM_OFF);
}

static uint32_t msg_key(mesh_msg_t const *m) {
    uint32_t h = scroll_key_hash(&m->heard_s, sizeof(m->heard_s), 0);
    h          = scroll_key_hash(&m->rssi_x10, sizeof(m->rssi_x10), h);
    h          = scroll_key_hash(m->channel, strlen(m->channel), h);
    return scroll_key_hash(m->text, strlen(m->text), h);
}

static void rebuild_messages(size_t count) {
    scroll_t s = save_scroll(messages_list, true, msg_keys, msg_keys_n);
    lv_obj_clean(messages_list);
    msg_keys_n = count;
    if (!count) {
        add_label(messages_list, "No messages yet. Listening on Public.", THEME_DIM);
        return;
    }
    uint32_t now = mesh_uptime_s();
    for (size_t i = 0; i < count; i++) {
        mesh_msg_t const *m = &msg_copy[i];
        char              age[24], line[220], meta[64];
        msg_keys[i]         = msg_key(m);
        mesh_format_age(now - m->heard_s, age, sizeof(age));
        snprintf(line, sizeof(line), "[%s] %s", m->channel, m->text);
        snprintf(meta, sizeof(meta), "%s, %d dBm", age, m->rssi_x10 / 10);
        lv_obj_t *item = add_item(messages_list);
        add_label(item, line, THEME_TEXT);
        add_label(item, meta, THEME_DIM);
    }
    restore_scroll(messages_list, s, true, msg_keys, msg_keys_n);
}

static void rebuild_contacts(size_t count) {
    scroll_t s = save_scroll(contacts_list, false, contact_keys, contact_keys_n);
    lv_obj_clean(contacts_list);
    contact_keys_n = count;
    if (!count) {
        add_label(contacts_list, "No nodes heard yet.", THEME_DIM);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        mesh_contact_t const *c = &contact_copy[i];
        char                  age[24], hops[16], meta[80];
        contact_keys[i]         = scroll_key_hash(c->pubkey, sizeof(c->pubkey), 0);
        mesh_format_age(c->age_s, age, sizeof(age));
        if (c->path_len == 0xFF) {
            snprintf(hops, sizeof(hops), "flood");
        } else if (c->path_len == 0) {
            snprintf(hops, sizeof(hops), "direct");
        } else {
            snprintf(hops, sizeof(hops), "%u hops", c->path_len);
        }
        snprintf(meta, sizeof(meta), "%s, %s, %s", mesh_type_name(c->type), hops, age);
        lv_obj_t *item = add_item(contacts_list);
        add_label(item, c->name[0] ? c->name : "(no name)", THEME_TEXT);
        add_label(item, meta, THEME_DIM);
    }
    restore_scroll(contacts_list, s, false, contact_keys, contact_keys_n);
}

static bool messages_visible(void) {
    return lv_tabview_get_tab_active(tabs) == TAB_MESSAGES;
}

// The copies are taken under the mesh lock (inside the snapshot functions), the widgets are built from them afterwards:
// the mesh task is never kept waiting on LVGL.
static void refresh(lv_timer_t *t) {
    (void)t;
    uint32_t generation;
    size_t   total_contacts;
    size_t   messages = mesh_messages_snapshot(msg_copy, SHOW_MESSAGES, &generation, SCREEN_WAIT_MS);
    if (messages == MESH_BUSY) {
        return; // MeshCore is busy (saving files): the screen stays as it is until the next tick
    }
    size_t contacts = mesh_contacts_snapshot(contact_copy, SHOW_CONTACTS, &total_contacts, SCREEN_WAIT_MS);
    if (contacts == MESH_BUSY) {
        return;
    }
    // The list changes when the count changes or someone is heard (again): the newest contact's hearing time
    uint32_t heard_at     = contacts ? mesh_uptime_s() - contact_copy[0].age_s : 0;
    uint32_t contacts_sig = (uint32_t)total_contacts * 2654435761u ^ heard_at;
    bool     stale        = mesh_uptime_s() - last_rebuild_s >= AGE_REFRESH_S;
    if (generation != shown_generation || stale) {
        rebuild_messages(messages);
        shown_generation = generation;
    }
    if (contacts_sig != shown_contacts_sig || stale) {
        rebuild_contacts(contacts);
        shown_contacts_sig = contacts_sig;
    }
    if (stale) {
        last_rebuild_s = mesh_uptime_s();
    }
    if (messages_visible()) {
        mesh_mark_seen(); // what is on screen now has been seen
    }
}

static lv_obj_t *active_list(void) {
    return messages_visible() ? messages_list : contacts_list;
}

static void switch_tab(int delta) {
    uint32_t n   = lv_tabview_get_tab_count(tabs);
    uint32_t cur = lv_tabview_get_tab_active(tabs);
    lv_tabview_set_active(tabs, (cur + n + delta) % n, LV_ANIM_OFF);
    if (messages_visible()) {
        mesh_mark_seen();
    }
}

static void on_key(lv_event_t *e) {
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT: switch_tab(-1); break;
        case LV_KEY_RIGHT: switch_tab(1); break;
        case LV_KEY_UP: shell_scroll_by(active_list(), SCROLL_STEP_PX); break;
        case LV_KEY_DOWN: shell_scroll_by(active_list(), -SCROLL_STEP_PX); break;
        default: break;
    }
}

// Tab (LV_KEY_NEXT) is taken by the group to move focus, and a group with one object doesn't move (nor tell anyone).
// With tab_hop as the second object the focus lands there: switch the tab and give the focus back to the tabs.
static void on_focus_move(lv_group_t *group) {
    if (tabs && tab_hop && lv_group_get_focused(group) == tab_hop) {
        switch_tab(1);
        lv_group_focus_obj(tabs); // calls this again, with the tabs focused: a no-op
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    heap_caps_free(msg_copy);
    heap_caps_free(contact_copy);
    msg_copy       = NULL;
    contact_copy   = NULL;
    heap_caps_free(anchor_mem);
    anchor_mem     = NULL;
    msg_keys       = NULL;
    contact_keys   = NULL;
    scroll_items   = NULL;
    msg_keys_n     = 0;
    contact_keys_n = 0;
    tabs           = NULL;
    tab_hop        = NULL;
    messages_list  = NULL;
    contacts_list  = NULL;
}

static lv_obj_t *make_list(lv_obj_t *tab) {
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tab, 10, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab, LV_SCROLLBAR_MODE_ACTIVE);
    shell_scroll_tame(tab);
    return tab;
}

static void style_tab_bar(lv_obj_t *bar) {
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_28, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(THEME_RULE), 0);
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *button = lv_obj_get_child(bar, (int32_t)i);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(button, lv_color_hex(THEME_DIM), 0);
        lv_obj_set_style_text_color(button, lv_color_hex(THEME_AMBER), LV_STATE_CHECKED);
        // Every tab has the underline, only the checked one shows it: otherwise its label jumps up (#20)
        lv_obj_set_style_border_side(button, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(button, 4, 0);
        lv_obj_set_style_border_width(button, 4, LV_STATE_CHECKED); // beats the theme's own CHECKED width
        // The theme also pads the top of the checked tab only; pad every tab like it, balancing the 4 px underline
        lv_obj_set_style_pad_top(button, 4, 0);
        lv_obj_set_style_pad_top(button, 4, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(button, lv_color_hex(THEME_AMBER), 0);
        lv_obj_set_style_border_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_STATE_CHECKED);
    }
}

static void build_tabs(lv_obj_t *screen, lv_group_t *group) {
    lv_obj_set_style_pad_bottom(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    msg_copy     = heap_caps_malloc(SHOW_MESSAGES * sizeof(*msg_copy), MALLOC_CAP_SPIRAM);
    contact_copy = heap_caps_malloc(SHOW_CONTACTS * sizeof(*contact_copy), MALLOC_CAP_SPIRAM);
    anchor_mem   = heap_caps_malloc(sizeof(*anchor_mem), MALLOC_CAP_SPIRAM);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    if (!msg_copy || !contact_copy || !anchor_mem) {
        ESP_LOGE(TAG, "no memory for the screen");
        lv_obj_t *label = add_label(screen, "Not enough memory to show MeshCore.", THEME_DIM);
        lv_obj_center(label);
        return;
    }

    msg_keys     = anchor_mem->msg;
    contact_keys = anchor_mem->contact;
    scroll_items = anchor_mem->items;

    tabs = lv_tabview_create(screen);
    lv_obj_set_size(tabs, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(tabs, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(tabs, 0, LV_STATE_FOCUS_KEY);
    lv_tabview_set_tab_bar_size(tabs, TAB_BAR_H);
    messages_list = make_list(lv_tabview_add_tab(tabs, "Messages"));
    contacts_list = make_list(lv_tabview_add_tab(tabs, "Contacts"));
    style_tab_bar(lv_tabview_get_tab_bar(tabs));
    lv_obj_set_scrollable(lv_tabview_get_content(tabs), false); // tabs change by key, not by swiping the content

    if (group) {
        lv_group_add_obj(group, tabs);
        tab_hop = lv_obj_create(screen);
        lv_obj_remove_style_all(tab_hop);
        lv_obj_set_size(tab_hop, 0, 0);
        lv_obj_set_floating(tab_hop, true);
        lv_obj_set_clickable(tab_hop, false);
        lv_group_add_obj(group, tab_hop);
        lv_group_set_focus_cb(group, on_focus_move);
        lv_group_focus_obj(tabs);
    }
    lv_obj_add_event_cb(tabs, on_key, LV_EVENT_KEY, NULL);

    shown_generation   = UINT32_MAX;
    shown_contacts_sig = UINT32_MAX;
    last_rebuild_s     = mesh_uptime_s();
    timer              = lv_timer_create(refresh, 1000, NULL);
    refresh(timer);
    mesh_mark_seen(); // opening on Messages: seen, even if the first snapshot found MeshCore busy
}

// ---- the first open: name, then region (#67) ---------------------------------------------------------------------
// The screen is on the stack from the start; the two questions open on top of it and come back to it. mc_first_action
// decides at every return: a question closed without an answer was Esc, so home. mc_name_set is saved only when both
// were answered, so the questions come back next open; an answered name stays applied.

static lv_obj_t  *first_screen; // NULL when no first-open flow is running
static mc_first_t first;

static void first_step(void *arg);

static void first_name_submitted(void) {
    mc_first_name_submitted(&first);
}

static void first_name_result(bool ok) {
    mc_first_name_result(&first, ok);
    first_step(NULL);
}

// A preset pops the list, Custom pops it and the list: back to this screen in one step, then on
static void first_resume(void *arg) {
    (void)arg;
    first_step(NULL);
}

static void first_region_chosen(void) {
    mc_first_region_chosen(&first);
    if (first_screen) {
        shell_back_to(first_screen, first_resume, NULL);
    }
}

static void first_home(void) {
    ESP_LOGW(TAG, "first-open questions left with Esc or not shown: asked again next open");
    meshcore_ask_cancel();
    first_screen = NULL;
    shell_home();
}

static void first_step(void *arg) {
    (void)arg;
    if (!first_screen || lv_screen_active() != first_screen) {
        return; // a question is open (or the screen is gone): it calls us again when it closes
    }
    switch (mc_first_action(&first)) {
        case MC_FIRST_WAIT: break;
        case MC_FIRST_ASK_NAME:
            mc_first_asked_name(&first);
            if (!meshcore_ask_name(first_name_submitted, first_name_result)) {
                first_home();
            }
            break;
        case MC_FIRST_ASK_REGION:
            mc_first_asked_region(&first);
            if (!meshcore_ask_region(first_region_chosen)) {
                first_home();
            }
            break;
        case MC_FIRST_HOME: first_home(); break;
        case MC_FIRST_FINISH: {
            bool saved = settings_set_u8("mc_name_set", 1);
            ESP_LOGW(TAG, "first-open questions answered (%s)", saved ? "saved" : "not saved");
            lv_obj_t   *screen = first_screen;
            lv_group_t *group  = lv_obj_get_group(screen);
            first_screen       = NULL;
            if (group) {
                lv_group_remove_obj(screen); // keys go to the tabs, Tab must not land on the bare screen
            }
            build_tabs(screen, group);
            break;
        }
    }
}

// LOADED comes while the shell is still switching screens: decide a moment later, after the closing question's own done
static void on_first_loaded(lv_event_t *e) {
    (void)e;
    lv_async_call(first_step, NULL);
}

static void on_first_delete(lv_event_t *e) {
    (void)e;
    meshcore_ask_cancel();
    first_screen = NULL;
}

static void open(lv_obj_t *screen) {
    if (mesh_running() && settings_get_u8("mc_name_set", 0) == 0) {
        ESP_LOGW(TAG, "first open: asking name and region");
        first        = (mc_first_t){0};
        first_screen = screen;
        lv_obj_add_event_cb(screen, on_first_loaded, LV_EVENT_SCREEN_LOADED, NULL);
        lv_obj_add_event_cb(screen, on_first_delete, LV_EVENT_DELETE, NULL);
        return;
    }
    build_tabs(screen, lv_group_get_default()); // the screen's own (shell_open)
}

// ---- Settings > MeshCore: about ----------------------------------------------------------------------------------

static lv_obj_t *about_radio; // the about rows' values that a radio change makes stale, NULL once the page is gone
static lv_obj_t *about_power;

static lv_obj_t *add_row(lv_obj_t *page, char const *key, char const *value) {
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, KEY_W);
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, value);
    return v;
}

// 62500 Hz is "62.5", 125000 Hz is "125"
static void fmt_khz(char *out, size_t cap, uint32_t hz) {
    if (hz % 1000 == 0) {
        snprintf(out, cap, "%" PRIu32, hz / 1000);
    } else {
        snprintf(out, cap, "%" PRIu32 ".%" PRIu32, hz / 1000, hz % 1000 / 100);
    }
}

static void radio_texts(char *text, size_t cap, char *power, size_t power_cap) {
    mesh_radio_t radio;
    if (mesh_radio_params(&radio, SCREEN_WAIT_MS)) {
        char khz[12];
        fmt_khz(khz, sizeof(khz), radio.bw_hz);
        snprintf(text, cap, "%" PRIu32 ".%03" PRIu32 " MHz  " LV_SYMBOL_BULLET "  %s kHz  " LV_SYMBOL_BULLET
                 "  SF%u  " LV_SYMBOL_BULLET "  CR%u",
                 radio.freq_hz / 1000000, radio.freq_hz % 1000000 / 1000, khz, radio.sf, radio.cr);
        snprintf(power, power_cap, "%d dBm", radio.tx_dbm);
    } else {
        strlcpy(text, "-", cap);
    }
}

void meshcore_about_refresh(void) {
    if (about_radio && about_power) {
        char text[96], power[16] = "-";
        radio_texts(text, sizeof(text), power, sizeof(power));
        lv_label_set_text(about_radio, text);
        lv_label_set_text(about_power, power);
    }
}

static void on_about_delete(lv_event_t *e) {
    (void)e;
    about_radio = NULL;
    about_power = NULL;
}

static void about(lv_obj_t *page) {
    menu_note(page, "About");
    char text[96];
    if (!mesh_node_id(text, sizeof(text))) {
        strlcpy(text, "-", sizeof(text)); // not running (the font has no em dash)
    }
    add_row(page, "node id", text);

    char power[16] = "-";
    radio_texts(text, sizeof(text), power, sizeof(power));
    about_radio = add_row(page, "radio", text);
    about_power = add_row(page, "TX power", power);
    lv_obj_add_event_cb(about_radio, on_about_delete, LV_EVENT_DELETE, NULL);
    snprintf(text, sizeof(text), "MeshCore %s", mesh_firmware_version());
    add_row(page, "version", text);
}

// ---- the background part -----------------------------------------------------------------------------------------

static bool start_background(void) {
    if (!lora_up()) {
        ESP_LOGE(TAG, "no LoRa radio: MeshCore not started");
        return false;
    }
    return mesh_start();
}

// The mesh first (it saves, and sends through LoRa), then the radio it uses
static void stop_for_update(void) {
    // lora_send (the mesh task) must not meet a LoRa task that is going away: if MeshCore didn't stop, LoRa stays
    if (mesh_stop_for_update()) {
        lora_stop();
    } else {
        ESP_LOGE(TAG, "MeshCore still running: LoRa left as it is");
    }
}

static lichen_app_t const meshcore_app = {
    .name             = "MeshCore",
    .draw_icon        = icon,
    .tile_text        = tile_text,
    .open             = open,
    .settings         = meshcore_settings,
    .about            = about,
    .start_background = start_background,
    .stop_for_update  = stop_for_update,
};

void meshcore_app_register(void) {
    lichen_register_app(&meshcore_app);
}
