// Home (#66, home.html): one wide live tile per app (default) or an icon grid. The home screen itself is the one
// focusable object; it takes the arrow keys, moves a selection with home_move() and opens the app on Enter. Tiles
// refresh their text once a second from the LVGL task, and every icon carries the red count circle of lichen_badge.h
// (#102), redrawn when a count changed.
//
// Arrange mode (#189): the person's own display order. Every per-app array here is indexed by the app's registry index
// (its identity for the shell, the badges and the notifications); order[slot] / slot_of[app] are only where it is
// shown. Settings > Display > Arrange home starts it: Enter picks the selected tile up, the arrows move it (insert and
// shift, the cells are reordered in place so the selection stays), Enter drops it and saves, Esc puts it back or ends
// arrange mode. The order is saved in badge_fw/home_order (tile_order.h) on a real change only.
#include "fw_console.h"
#include "lichen_mem.h"
#include "esp_log.h"
#include "fonts.h"
#include "home_layout.h"
#include "lichen_app.h"
#include "lichen_sys_app.h"
#include "lichen_badge.h"
#include "lichen_sys_badge.h"
#include "lichen_notify.h"
#include "lichen_sys_ui.h"
#include "lichen_ui.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"
#include "tile_order.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(HOME_KEY_UP == LV_KEY_UP && HOME_KEY_DOWN == LV_KEY_DOWN && HOME_KEY_LEFT == LV_KEY_LEFT &&
                  HOME_KEY_RIGHT == LV_KEY_RIGHT,
              "home_layout.h key values must match LVGL's");

static char const TAG[] = "home";

#define TILE_ICON_PX  32
#define TILE_GAP      16
#define TILE_RADIUS   20
#define LINE_CAP      64
#define REFRESH_MS    1000
#define BADGE_RED     THEME_LINK_RED // the count circle (#102): white on red, like a phone's app badge
#define BADGE_PAD_HOR 6
#define HOME_ID_MAX   32  // an app id's length, generously (app_ns.h allows 13)
#define ORDER_CAP     800 // "1:" + LICHEN_MAX_APPS ids with their commas + NUL; a longer stored value reads as default
#define ORDER_LINE_APPS 6 // per "fw home" reply line: far under the console's 384-byte log line
static_assert(ORDER_CAP >= 2 + LICHEN_MAX_APPS * (HOME_ID_MAX + 1) + 1, "ORDER_CAP holds every app's id");

typedef struct {
    home_layout_t layout;
    int           count;
    int           sel;                    // the selected app (a registry index, like every array here)
    int           order[LICHEN_MAX_APPS]; // order[slot] = the app shown there
    int           slot_of[LICHEN_MAX_APPS];
    int           picked[LICHEN_MAX_APPS]; // the order when the tile was picked up: Esc puts it back
    bool          arranging;
    bool          holding;
    lv_obj_t     *moving; // the amber "Moving" label on the held tile
    lv_obj_t     *cell[LICHEN_MAX_APPS];
    lv_obj_t     *line1[LICHEN_MAX_APPS]; // tiles only
    lv_obj_t     *line2[LICHEN_MAX_APPS];
    lv_obj_t     *badge[LICHEN_MAX_APPS]; // the count circle on each icon (#102), hidden at 0
    lv_obj_t     *icon[LICHEN_MAX_APPS];  // the box it sits on
    uint32_t      badge_gen;              // lichen_badge_generation() when they were last drawn
    bool          badges_drawn;
    lv_timer_t   *timer;
} home_t;

// The app selected last, over rebuilds: a layout switch keeps it (#189). By id, not registry index, so a change in
// the registration order still finds it. NULL: the first tile. App structs live as long as the badge runs.
static char const *last_app;

static int app_index(char const *id) {
    for (size_t i = 0; id && i < lichen_app_count(); i++) {
        if (strcmp(lichen_app_at(i)->id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void style_cell(home_t const *h, int i) {
    bool selected = i == h->sel;
    lv_obj_set_style_bg_color(h->cell[i], lv_color_hex(selected ? THEME_TRACK : THEME_TILE), 0);
    // Fixed width, only the opacity changes: a border that comes and goes shifts the cell's contents (#20)
    lv_obj_set_style_border_width(h->cell[i], 3, 0);
    lv_obj_set_style_border_opa(h->cell[i], selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void select(home_t *h, int sel) {
    int old = h->sel;
    h->sel  = sel;
    last_app = lichen_app_at((size_t)sel)->id;
    style_cell(h, old);
    style_cell(h, sel);
    lv_obj_scroll_to_view(h->cell[sel], LV_ANIM_ON);
}

static void open_selected(void *arg) {
    shell_open_app((size_t)(intptr_t)arg);
}

// ---- The saved order (#189) ----------------------------------------------------------------------------------------

static void app_ids(char const **ids, int n) {
    for (int i = 0; i < n; i++) {
        ids[i] = lichen_app_at((size_t)i)->id;
    }
}

// order gets the saved order merged with the apps registered now; true when a saved order was used
static bool load_order(int *order, int n) {
    char const *ids[LICHEN_MAX_APPS];
    app_ids(ids, n);
    char *text = lichen_alloc(lichen_sys_ctx_os(), ORDER_CAP, LICHEN_MEM_BULK); // the console task's stack is small too
    if (text) {
        settings_get_str(HOME_ORDER_KEY, text, ORDER_CAP, "");
    }
    bool used = tile_order_merge(text, ids, n, order);
    lichen_free(lichen_sys_ctx_os(), text);
    return used;
}

// The registration order is not stored: back to it, the key goes, as for someone who never arranged
static bool save_order(int const *order, int n) {
    if (tile_order_is_default(order, n)) {
        return settings_erase(HOME_ORDER_KEY);
    }
    char const *ids[LICHEN_MAX_APPS];
    app_ids(ids, n);
    char *text = lichen_alloc(lichen_sys_ctx_os(), ORDER_CAP, LICHEN_MEM_BULK);
    bool  ok   = text && tile_order_encode(ids, order, n, text, ORDER_CAP) > 0 && settings_set_str(HOME_ORDER_KEY, text);
    lichen_free(lichen_sys_ctx_os(), text);
    if (!ok) {
        ESP_LOGE(TAG, "home order not saved");
    }
    return ok;
}

// ---- Arrange mode (#189) -------------------------------------------------------------------------------------------

static void arrange_hint(home_t *h, lv_obj_t *home) {
    if (h->holding) {
        shell_page_hint(home, "Arrows", "move", "Enter", "drop", "Esc", "cancel", NULL);
    } else {
        shell_page_hint(home, "Enter", "pick up", "Esc", "done", NULL);
    }
}

// The amber "Moving" on the held tile: in the tile's row after its text, or under the grid cell's name
static void show_moving(home_t *h, bool on) {
    if (h->moving) {
        lv_obj_delete(h->moving);
        h->moving = NULL;
    }
    if (!on) {
        return;
    }
    lv_obj_t *cell  = h->cell[h->sel];
    lv_obj_t *label = lichen_label_line(cell, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, LV_PCT(h->layout == HOME_GRID ? 100 : 40), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(label, "Moving");
    if (h->layout == HOME_GRID) {
        // Floating in the cell's bottom pad, under the name (tile_cell_size keeps a line for it): in the flex flow it
        // would push the icon up, away from its badge. The pad lies outside the content area the alignment uses, so
        // its height is added back (as meshcore_grid.c's)
        int32_t pad = lv_obj_get_style_pad_bottom(cell, LV_PART_MAIN);
        lv_obj_set_floating(label, true);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, pad - TILE_GRID_MOVING_EDGE);
    }
    h->moving = label;
}

// The cells in the children's order of order[] (after a cancel); the hint, floating and last, stays where it is
static void place_cells(home_t *h) {
    for (int slot = 0; slot < h->count; slot++) {
        lv_obj_move_to_index(h->cell[h->order[slot]], slot);
    }
}

static void scroll_to_sel(home_t *h) {
    lv_obj_update_layout(lv_obj_get_parent(h->cell[h->sel]));
    lv_obj_scroll_to_view(h->cell[h->sel], LV_ANIM_ON);
}

static void move_held(home_t *h, int from, int to) {
    if (!tile_order_move(h->order, h->count, from, to)) {
        return;
    }
    tile_order_slots(h->order, h->count, h->slot_of);
    lv_obj_move_to_index(h->cell[h->sel], to); // insert and shift, as the order: no rebuild, the selection stays
    scroll_to_sel(h);
}

static void cancel_hold(home_t *h, lv_obj_t *home) {
    memcpy(h->order, h->picked, sizeof(h->order));
    tile_order_slots(h->order, h->count, h->slot_of);
    place_cells(h);
    h->holding = false;
    show_moving(h, false);
    arrange_hint(h, home);
    scroll_to_sel(h);
}

static void hold(home_t *h, lv_obj_t *home, bool on) {
    bool moved = !on && memcmp(h->picked, h->order, (size_t)h->count * sizeof(*h->order)) != 0;
    // A real move: only now does anything reach NVS. A failed save must not look saved: the tile goes back
    if (moved && !save_order(h->order, h->count)) {
        cancel_hold(h, home);
        lichen_notify(lichen_sys_ctx_os(), LN_WARN, "Home order not saved",
                      "The tile went back to its place. Try again; if it keeps failing, restart the badge.", 0);
        return;
    }
    if (on) {
        memcpy(h->picked, h->order, sizeof(h->picked));
    }
    h->holding = on;
    show_moving(h, on);
    arrange_hint(h, home);
}

void home_arrange(lv_obj_t *home, bool on) {
    home_t *h = home ? lv_obj_get_user_data(home) : NULL;
    if (!h || h->count == 0 || on == h->arranging) {
        return;
    }
    if (h->holding) {
        cancel_hold(h, home);
    }
    h->arranging = on;
    if (on) {
        arrange_hint(h, home);
        scroll_to_sel(h);
        return;
    }
    lv_obj_t *hint = lichen_page_hint_of(home);
    if (hint) {
        lv_obj_delete(hint);
    }
    lv_obj_set_style_pad_bottom(home, THEME_PAD, 0); // as home_create left it
}

bool home_arrange_esc(void *arg) {
    lv_obj_t *home = arg;
    home_t   *h    = lv_obj_get_user_data(home);
    if (!h || !h->arranging) {
        return false;
    }
    if (h->holding) {
        cancel_hold(h, home);
    } else {
        home_arrange(home, false);
    }
    return true;
}

static void on_key(lv_event_t *e) {
    home_t   *h    = lv_event_get_user_data(e);
    lv_obj_t *home = lv_event_get_current_target(e);
    uint32_t  key  = lv_event_get_key(e);
    if (h->count == 0) {
        return;
    }
    if (key == LV_KEY_ENTER && h->arranging) {
        hold(h, home, !h->holding); // never opens an app while arranging
        return;
    }
    if (key == LV_KEY_ENTER) {
        // Outside the indev's key handling: opening swaps the indev's group
        lichen_sys_ui_call(open_selected, (void *)(intptr_t)h->sel, 0);
        return;
    }
    int from = h->slot_of[h->sel];
    int to   = home_move(h->layout, h->count, from, key);
    if (to == from) {
        return;
    }
    if (h->holding) {
        move_held(h, from, to);
    } else {
        select(h, h->order[to]);
    }
}

static void on_delete(lv_event_t *e) {
    home_t *h = lv_event_get_user_data(e);
    lv_timer_delete(h->timer);
    lichen_free(lichen_sys_ctx_os(), h);
}

// Centred on the icon's top-right corner, so it overlaps the icon by half, phone style. Its width follows the text, so
// again after every new text, and after home has laid out (align_to takes the positions of that moment).
static void place_badge(lv_obj_t *badge, lv_obj_t *icon) {
    int32_t h = lv_font_get_line_height(lv_obj_get_style_text_font(badge, LV_PART_MAIN));
    lv_obj_update_layout(badge);
    lv_obj_align_to(badge, icon, LV_ALIGN_TOP_RIGHT, h / 2, -h / 2);
}

static void refresh_badge(lv_obj_t *badge, lv_obj_t *icon, unsigned count) {
    char text[8];
    bool shown = home_badge_text(count, text, sizeof(text));
    if (shown && strcmp(lv_label_get_text(badge), text) != 0) {
        lv_label_set_text(badge, text);
        place_badge(badge, icon);
    }
    if (shown == lv_obj_is_hidden(badge)) {
        lv_obj_set_hidden(badge, !shown);
    }
}

// Only when a count changed somewhere: a lookup per app, no app code
static void refresh_badges(home_t *h) {
    uint32_t generation = lichen_badge_generation();
    if (h->badges_drawn && generation == h->badge_gen) {
        return;
    }
    for (int i = 0; i < h->count; i++) {
        if (h->badge[i]) {
            refresh_badge(h->badge[i], h->icon[i], lichen_sys_badge_get(lichen_app_at(i)->id));
        }
    }
    h->badge_gen    = generation;
    h->badges_drawn = true;
}

static void refresh(lv_timer_t *timer) {
    home_t *h = lv_timer_get_user_data(timer);
    refresh_badges(h);
    for (int i = 0; i < h->count; i++) {
        lichen_app_t const *app = lichen_app_at(i);
        char                t1[LINE_CAP], t2[LINE_CAP];
        if (!h->line1[i] || !app->tile_text) {
            continue;
        }
        t1[0] = t2[0] = '\0';
        lichen_ctx_t previous = lichen_ctx_enter(lichen_sys_app_ctx(app)); // #230: the hook runs for its app
        bool         fresh    = app->tile_text(t1, sizeof(t1), t2, sizeof(t2));
        lichen_ctx_leave(previous);
        if (!fresh) {
            continue; // nothing new: keep what is shown
        }
        if (strcmp(lv_label_get_text(h->line1[i]), t1) != 0) {
            lv_label_set_text(h->line1[i], t1);
        }
        if (strcmp(lv_label_get_text(h->line2[i]), t2) != 0) {
            lv_label_set_text(h->line2[i], t2);
        }
    }
}

// A child of the cell, not of the icon box: the box clips what sticks out of it (an overflow-visible box still showed
// only a quarter of the circle on the badge). Floating, so the cell's flex layout leaves it where place_badge puts it;
// last child, so it is drawn over the icon. Hidden until refresh() has a count.
static lv_obj_t *add_badge(lv_obj_t *cell) {
    lv_font_t const *font  = lichen_font(LICHEN_FONT_SMALL); // the smallest Lichen font, also on the 32 px tile icon
    int32_t          h     = lv_font_get_line_height(font);
    lv_obj_t        *badge = lv_label_create(cell);
    lv_obj_set_floating(badge, true);
    lv_obj_set_style_text_font(badge, font, 0);
    lv_obj_set_style_text_color(badge, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_align(badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(BADGE_RED), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(badge, BADGE_PAD_HOR, 0);
    lv_obj_set_style_min_width(badge, h, 0);
    lv_obj_set_height(badge, h);
    lv_obj_set_clickable(badge, false);
    lv_label_set_text(badge, "");
    lv_obj_set_hidden(badge, true);
    return badge;
}

static lv_obj_t *add_icon(lv_obj_t *parent, lichen_app_t const *app, int px) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, px, px);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    if (app->draw_icon) {
        lichen_ctx_t previous = lichen_ctx_enter(lichen_sys_app_ctx(app));
        app->draw_icon(box, px);
        lichen_ctx_leave(previous);
    } else {
        // An app without an icon gets its initial in a rounded square
        lv_obj_set_style_radius(box, px / 4, 0);
        lv_obj_set_style_bg_color(box, lv_color_hex(THEME_FAINT), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_t *initial = lv_label_create(box);
        lv_label_set_text_fmt(initial, "%c", app->name[0]);
        lv_obj_center(initial);
    }
    return box;
}

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lichen_label_line(parent, LV_PCT(100)); // without a width DOT never has anything to cut (#132)
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *base_cell(lv_obj_t *parent) {
    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell, TILE_RADIUS, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_scrollable(cell, false);
    lv_obj_set_clickable(cell, false);
    return cell;
}

static void add_tile(home_t *h, lv_obj_t *home, int i) {
    lichen_app_t const *app  = lichen_app_at(i);
    lv_obj_t           *tile = base_cell(home);
    lv_obj_set_size(tile, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(tile, 20, 0);
    lv_obj_set_style_pad_column(tile, 24, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    h->icon[i] = add_icon(tile, app, TILE_ICON_PX);

    lv_obj_t *text = lv_obj_create(tile);
    lv_obj_remove_style_all(text);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, 4, 0);
    lv_obj_set_scrollable(text, false);
    lv_obj_set_clickable(text, false);
    add_label(text, app->name, THEME_TEXT);
    h->line1[i] = add_label(text, "", THEME_DIM);
    h->line2[i] = add_label(text, "", THEME_DIM);
    lv_obj_set_width(h->line1[i], LV_PCT(100));
    lv_obj_set_width(h->line2[i], LV_PCT(100));
    h->badge[i] = add_badge(tile); // after the text, so it is drawn over everything in the tile
    h->cell[i]  = tile;
}

static void add_grid_cell(home_t *h, lv_obj_t *home, int i) {
    lichen_app_t const *app  = lichen_app_at(i);
    lv_obj_t           *cell = base_cell(home);
    // The same cell as MeshCore's grid (#193), sized from the font (#28)
    int32_t     line_h = lv_font_get_line_height(lv_obj_get_style_text_font(cell, LV_PART_MAIN));
    tile_cell_t size   = tile_cell_size(LV_HOR_RES - 2 * THEME_PAD, HOME_GRID_COLS, line_h);
    lv_obj_set_style_pad_top(cell, TILE_GRID_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(cell, size.pad_bottom, 0);
    lv_obj_set_size(cell, size.w, size.h);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, TILE_GRID_ICON_GAP, 0);
    h->icon[i] = add_icon(cell, app, TILE_GRID_ICON_PX);
    lv_obj_t *name = add_label(cell, app->name, THEME_TEXT);
    lv_obj_set_width(name, size.w - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    h->badge[i] = add_badge(cell);
    h->cell[i]  = cell;
}

// "fw ui": the home screen itself holds the focus, so the selection has to be asked for
char const *home_selected_name(lv_obj_t *home) {
    home_t const *h = lv_obj_get_user_data(home);
    if (!h || h->count == 0 || h->sel >= h->count) {
        return NULL;
    }
    return lichen_app_at(h->sel)->name;
}

lv_obj_t *home_create(void) {
    lv_obj_t *home = lv_obj_create(NULL);
    shell_style_screen(home);
    lv_obj_set_style_pad_top(home, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_bottom(home, THEME_PAD, 0);
    lv_obj_set_style_pad_hor(home, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(home, LV_SCROLLBAR_MODE_OFF);
    // The arrows move the selection, and select() scrolls it into view. LVGL's own arrow scrolling (on by default)
    // would first jump the screen a quarter down on Right (no horizontal scroll, so it scrolls vertically) and
    // select() then scroll it back: the icons jumping on the first row (#124)
    lv_obj_remove_flag(home, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    // The screen is the group's focus object: no focus ring around all of it
    lv_obj_set_style_outline_width(home, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(home, 0, LV_STATE_FOCUS_KEY);

    home_t *h = lichen_alloc(lichen_sys_ctx_os(), sizeof(*h), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!h) {
        return home;
    }
    h->count  = (int)lichen_app_count();
    h->layout = home_layout_from_u8(settings_get_u8("home_layout", HOME_TILES));
    if (h->count == 0) {
        lv_obj_t *empty = add_label(home, "No apps yet", THEME_DIM);
        lv_obj_center(empty);
    } else if (h->layout == HOME_GRID) {
        lv_obj_set_flex_flow(home, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_row(home, TILE_GRID_ROW_GAP, 0);
        lv_obj_set_style_pad_column(home, TILE_GRID_COL_GAP, 0);
    } else {
        lv_obj_set_flex_flow(home, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(home, TILE_GAP, 0);
    }
    lv_obj_set_user_data(home, h);
    load_order(h->order, h->count);
    tile_order_slots(h->order, h->count, h->slot_of);
    int kept = app_index(last_app);
    h->sel   = kept >= 0 && kept < h->count ? kept : h->order[0];
    for (int slot = 0; slot < h->count; slot++) {
        int app = h->order[slot];
        if (h->layout == HOME_GRID) {
            add_grid_cell(h, home, app);
        } else {
            add_tile(h, home, app);
        }
        style_cell(h, app);
    }
    lv_obj_update_layout(home); // the icons' final places, for the badges
    for (int i = 0; i < h->count; i++) {
        place_badge(h->badge[i], h->icon[i]);
    }
    if (h->count > 0 && h->slot_of[h->sel] > 0) {
        lv_obj_scroll_to_view(h->cell[h->sel], LV_ANIM_OFF); // a kept selection further down shows at once
    }
    h->timer = lv_timer_create(refresh, REFRESH_MS, h);
    refresh(h->timer); // no blank lines or missing bubbles for the first second
    lv_obj_add_event_cb(home, on_key, LV_EVENT_KEY, h);
    lv_obj_add_event_cb(home, on_delete, LV_EVENT_DELETE, h);
    return home;
}

// ---- "fw home": the console side of Settings > Display > Home layout, Arrange home and Reset order ---------------

bool home_order_arranged(void) {
    int n = (int)lichen_app_count();
    int order[LICHEN_MAX_APPS];
    return load_order(order, n) && !tile_order_is_default(order, n);
}

// The whole order, ORDER_LINE_APPS ids a line, so no line is cut in the log or the Wi-Fi console
static void reply_order(void) {
    int  n = (int)lichen_app_count();
    int  order[LICHEN_MAX_APPS];
    bool arranged = load_order(order, n) && !tile_order_is_default(order, n);
    console_reply("home order (%s), %d apps:", arranged ? "arranged" : "default", n);
    for (int first = 0; first < n; first += ORDER_LINE_APPS) {
        char   line[ORDER_LINE_APPS * (HOME_ID_MAX + 2) + 1];
        size_t len  = 0;
        int    last = first + ORDER_LINE_APPS < n ? first + ORDER_LINE_APPS : n;
        line[0]     = '\0';
        for (int slot = first; slot < last && len < sizeof(line); slot++) {
            int w = snprintf(line + len, sizeof(line) - len, "%s%s", slot > first ? ", " : "",
                             lichen_app_at((size_t)order[slot])->id);
            len += w > 0 ? (size_t)w : 0;
        }
        console_reply("  %d-%d: %s", first + 1, last, line);
    }
}

static void cmd_home(char const *args) {
    if (strcmp(args, "layout tiles") == 0 || strcmp(args, "layout grid") == 0) {
        bool grid = strcmp(args + 7, "grid") == 0;
        settings_set_u8("home_layout", grid ? HOME_GRID : HOME_TILES);
        shell_rebuild_home();
        console_reply("home layout: %s (saved)", grid ? "grid" : "tiles");
    } else if (strcmp(args, "order reset") == 0) {
        settings_erase(HOME_ORDER_KEY);
        shell_rebuild_home_below();
        reply_order();
    } else if (strcmp(args, "order") == 0) {
        reply_order();
    } else {
        // tools/ui_overflow_sweep.py reads "layout is tiles" from this first line
        console_reply("home layout tiles|grid | home order [reset]  (layout is %s)",
                      settings_get_u8("home_layout", 0) == HOME_GRID ? "grid" : "tiles");
        reply_order();
    }
}

void home_init(void) {
    console_register("home",
                     "home layout tiles|grid | home order [reset] (the home screen's layout and tile order, as in "
                     "Settings > Display)",
                     cmd_home);
    ESP_LOGI(TAG, "ready");
}
