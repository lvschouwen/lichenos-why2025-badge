// Home (#66, home.html): one wide live tile per app (default) or an icon grid. The home screen itself is the one
// focusable object; it takes the arrow keys, moves a selection with home_move() and opens the app on Enter. Tiles
// refresh their text once a second from the LVGL task.
#include "fw_console.h"
#include "esp_log.h"
#include "home_layout.h"
#include "lichen_app.h"
#include "settings_store.h"
#include "shell_parts.h"
#include "theme.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static_assert(HOME_KEY_UP == LV_KEY_UP && HOME_KEY_DOWN == LV_KEY_DOWN && HOME_KEY_LEFT == LV_KEY_LEFT &&
                  HOME_KEY_RIGHT == LV_KEY_RIGHT,
              "home_layout.h key values must match LVGL's");

static char const TAG[] = "home";

#define TILE_ICON_PX  32
#define GRID_ICON_PX  64
#define TILE_GAP      16
#define TILE_RADIUS   20
#define GRID_GAP      24
#define GRID_CELL_H   176
#define LINE_CAP      64
#define REFRESH_MS    1000

typedef struct {
    home_layout_t layout;
    int           count;
    int           sel;
    lv_obj_t     *cell[LICHEN_MAX_APPS];
    lv_obj_t     *line1[LICHEN_MAX_APPS]; // tiles only
    lv_obj_t     *line2[LICHEN_MAX_APPS];
    lv_timer_t   *timer;
} home_t;

static void style_cell(home_t const *h, int i) {
    bool selected = i == h->sel;
    lv_obj_set_style_bg_color(h->cell[i], lv_color_hex(selected ? THEME_TRACK : THEME_TILE), 0);
    lv_obj_set_style_border_width(h->cell[i], selected ? 3 : 0, 0);
}

static void select(home_t *h, int sel) {
    int old = h->sel;
    h->sel  = sel;
    style_cell(h, old);
    style_cell(h, sel);
    lv_obj_scroll_to_view(h->cell[sel], LV_ANIM_ON);
}

static void open_selected(void *arg) {
    shell_open_app((size_t)(intptr_t)arg);
}

static void on_key(lv_event_t *e) {
    home_t  *h   = lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) {
        // Outside the indev's key handling: opening swaps the indev's group
        lv_async_call(open_selected, (void *)(intptr_t)h->sel);
        return;
    }
    int sel = home_move(h->layout, h->count, h->sel, key);
    if (sel != h->sel) {
        select(h, sel);
    }
}

static void on_delete(lv_event_t *e) {
    home_t *h = lv_event_get_user_data(e);
    lv_timer_delete(h->timer);
    free(h);
}

static void refresh(lv_timer_t *timer) {
    home_t *h = lv_timer_get_user_data(timer);
    for (int i = 0; i < h->count; i++) {
        lichen_app_t const *app = lichen_app_at(i);
        char                t1[LINE_CAP], t2[LINE_CAP];
        if (!h->line1[i] || !app->tile_text) {
            continue;
        }
        t1[0] = t2[0] = '\0';
        if (!app->tile_text(t1, sizeof(t1), t2, sizeof(t2))) {
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

static lv_obj_t *add_icon(lv_obj_t *parent, lichen_app_t const *app, int px) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, px, px);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    if (app->draw_icon) {
        app->draw_icon(box, px);
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
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
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
    add_icon(tile, app, TILE_ICON_PX);

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
    h->cell[i] = tile;
}

static void add_grid_cell(home_t *h, lv_obj_t *home, int i) {
    lichen_app_t const *app  = lichen_app_at(i);
    lv_obj_t           *cell = base_cell(home);
    int                 w    = (LV_HOR_RES - 2 * THEME_PAD - (HOME_GRID_COLS - 1) * GRID_GAP) / HOME_GRID_COLS;
    lv_obj_set_size(cell, w, GRID_CELL_H);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 14, 0);
    add_icon(cell, app, GRID_ICON_PX);
    lv_obj_t *name = add_label(cell, app->name, THEME_TEXT);
    lv_obj_set_width(name, w - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    h->cell[i] = cell;
}

lv_obj_t *home_create(void) {
    lv_obj_t *home = lv_obj_create(NULL);
    shell_style_screen(home);
    lv_obj_set_style_pad_top(home, THEME_STATUS_BAR_H + THEME_PAD, 0);
    lv_obj_set_style_pad_bottom(home, THEME_PAD, 0);
    lv_obj_set_style_pad_hor(home, THEME_PAD, 0);
    lv_obj_set_scrollbar_mode(home, LV_SCROLLBAR_MODE_OFF);
    // The screen is the group's focus object: no focus ring around all of it
    lv_obj_set_style_outline_width(home, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(home, 0, LV_STATE_FOCUS_KEY);

    home_t *h = calloc(1, sizeof(*h));
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
        lv_obj_set_style_pad_row(home, GRID_GAP, 0);
        lv_obj_set_style_pad_column(home, GRID_GAP, 0);
    } else {
        lv_obj_set_flex_flow(home, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(home, TILE_GAP, 0);
    }
    for (int i = 0; i < h->count; i++) {
        if (h->layout == HOME_GRID) {
            add_grid_cell(h, home, i);
        } else {
            add_tile(h, home, i);
        }
        style_cell(h, i);
    }
    h->timer = lv_timer_create(refresh, REFRESH_MS, h);
    if (h->layout == HOME_TILES) {
        refresh(h->timer); // no blank lines for the first second
    }
    lv_obj_add_event_cb(home, on_key, LV_EVENT_KEY, h);
    lv_obj_add_event_cb(home, on_delete, LV_EVENT_DELETE, h);
    return home;
}

// ---- "fw home layout": the console setter of Settings > Display > Home layout ------------------------------------

static void cmd_home(char const *args) {
    if (strcmp(args, "layout tiles") == 0 || strcmp(args, "layout grid") == 0) {
        bool grid = strcmp(args + 7, "grid") == 0;
        settings_set_u8("home_layout", grid ? HOME_GRID : HOME_TILES);
        shell_rebuild_home();
        console_reply("home layout: %s (saved)", grid ? "grid" : "tiles");
    } else {
        console_reply("home layout tiles|grid  (layout is %s)",
                      settings_get_u8("home_layout", 0) == HOME_GRID ? "grid" : "tiles");
    }
}

void home_init(void) {
    console_register("home", "home layout tiles|grid (the home screen's layout, as in Settings > Display)", cmd_home);
    ESP_LOGI(TAG, "ready");
}
