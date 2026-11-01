// The MeshCore app's home grid (#88): six tiles in Lichen home's grid look (home.c add_grid_cell) and key rules
// (mesh_grid_move = home_move). Each tile is a focusable object in the screen's group, so "fw ui" names the focused
// tile; the arrows move the focus, Enter opens the tile's screen on the shell's stack (Esc there comes back here) and
// Esc here is the shell's: back to Lichen home. Badges refresh once a second from copies (mesh.h), never waiting on
// MeshCore for more than a tick.
#include "esp_log.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_grid.h"
#include "meshcore_screens.h"
#include "meshcore_settings.h"
#include "theme.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static_assert(MESH_GRID_KEY_UP == LV_KEY_UP && MESH_GRID_KEY_DOWN == LV_KEY_DOWN && MESH_GRID_KEY_LEFT == LV_KEY_LEFT &&
                  MESH_GRID_KEY_RIGHT == LV_KEY_RIGHT,
              "mesh_grid.h key values must match LVGL's");

static char const TAG[] = "meshcore";

#define ICON_PX        64 // home.c GRID_ICON_PX and the rest of its grid sizes
#define GRID_GAP       24
#define CELL_PAD       32
#define ICON_GAP       14
#define TILE_RADIUS    20
#define REFRESH_MS     1000
#define SCREEN_WAIT_MS 20
#define ADVERT_ASK_S   15 // the confirm cancels by itself after this: nothing goes on air unasked
#define STATUS_KEEP_MS 10000 // the Advertise result stays this long under the grid

static struct {
    lv_obj_t          *screen;
    lv_obj_t          *cell[MESH_GRID_TILES];
    lv_obj_t          *badge[MESH_GRID_TILES];
    lv_obj_t          *status; // the Advertise result; NULL once the grid is gone
    lv_timer_t        *timer;
    lv_timer_t        *status_timer; // clears the status line

    mesh_grid_counts_t counts;
} g;

static void set_text(lv_obj_t *label, char const *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text); // only on a change: the same text every second would redraw for nothing
    }
}

// The counts that need the mesh lock, in one pass; a busy MeshCore keeps the last ones
static void take_contact_counts(mesh_grid_counts_t *c) {
    mesh_contact_stats_t st;
    if (mesh_contacts_stats(&st, SCREEN_WAIT_MS)) {
        c->contacts    = st.total;
        c->repeaters   = st.repeaters;
        c->heard_any   = st.heard_any;
        c->heard_age_s = st.newest_age_s;
    }
}

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != g.screen) {
        return; // a tile's screen is on top: no mesh lock for badges nobody sees
    }
    mesh_grid_counts_t *c = &g.counts;
    if (mesh_running()) {
        mesh_tile_t tile;
        mesh_tile_snapshot(&tile); // never waits: the home tile's own copy
        c->unread       = tile.unread_total;
        c->advert_sent  = tile.advert_sent;
        c->advert_age_s = tile.advert_age_s;
        take_contact_counts(c);
    } else {
        *c = (mesh_grid_counts_t){0};
    }
    for (int i = 0; i < MESH_GRID_TILES; i++) {
        char text[MESH_GRID_BADGE_MAX];
        mesh_grid_badge((mesh_grid_tile_t)i, c, text, sizeof(text));
        set_text(g.badge[i], text);
    }
}

// ---- Advertise: confirmed first, the result under the grid ----------------------------------------------------------

static void clear_status(lv_timer_t *t) {
    (void)t;
    set_text(g.status, "");
    if (g.status_timer) {
        lv_timer_pause(g.status_timer); // until the next result
    }
}

// "Advert: <text>", cleared STATUS_KEEP_MS after the last one (or when a tile opens)
static void show_status(char const *text) {
    if (!g.status) {
        return;
    }
    char line[64];
    snprintf(line, sizeof(line), "Advert: %s", text);
    set_text(g.status, line);
    if (g.status_timer) {
        lv_timer_reset(g.status_timer);
        lv_timer_resume(g.status_timer);
    }
}

static void advert_status(char const *text) {
    show_status(text);
    if (g.status) {
        refresh(NULL); // the badge's age starts now
    }
}

static void advert_answer(bool confirmed) {
    if (!g.status) {
        return; // the grid went away meanwhile
    }
    if (!confirmed) {
        show_status("not sent");
        return;
    }
    ESP_LOGW(TAG, "advert confirmed on the grid");
    meshcore_advert_send(advert_status);
}

static void ask_advert(void) {
    if (!mesh_running()) {
        show_status("MeshCore is not running");
        return;
    }
    char const *refusal = mesh_advert_refusal(clock_is_set());
    if (refusal) {
        advert_status(refusal); // nothing to confirm: it would not be sent
        return;
    }
    shell_dialog_t const d = {
        .title        = "Send an advert now? Nodes in direct range hear it.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: send    Esc: cancel",
        .seconds      = ADVERT_ASK_S,
        .careful      = true, // a held Enter never sends it, the countdown ends in a cancel
    };
    if (!shell_dialog_countdown(&d, advert_answer)) {
        show_status("busy, try again");
    }
}

// ---- keys --------------------------------------------------------------------------------------------------------

static void open_tile(void *arg) {
    clear_status(NULL); // an old result does not belong to what comes next
    switch ((mesh_grid_tile_t)(intptr_t)arg) {
        case MESH_GRID_CHAT: meshcore_chats_open(); break;
        case MESH_GRID_CONTACTS: meshcore_contacts_open(MESH_VIEW_ALL); break;
        case MESH_GRID_REPEATERS: meshcore_contacts_open(MESH_VIEW_REPEATERS); break;
        case MESH_GRID_HEARD: meshcore_contacts_open(MESH_VIEW_HEARD); break;
        case MESH_GRID_ADVERTISE: ask_advert(); break;
        case MESH_GRID_SETTINGS: meshcore_app_settings_open(); break;
        default: break;
    }
}

static void on_key(lv_event_t *e) {
    int      at  = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER) {
        // Outside the indev's key handling: opening swaps the indev's group (as home.c does)
        lv_async_call(open_tile, (void *)(intptr_t)at);
        return;
    }
    int to = mesh_grid_move(MESH_GRID_TILES, at, key);
    if (to != at) {
        lv_group_focus_obj(g.cell[to]);
        lv_obj_scroll_to_view(g.cell[to], LV_ANIM_OFF);
    }
}

// ---- the screen --------------------------------------------------------------------------------------------------

// Lichen home's grid cell: tile colour, an amber frame and the selected colour while focused. The frame's width is
// fixed and only its opacity changes, so the contents never shift (#20).
static void style_cell(lv_obj_t *cell) {
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cell, TILE_RADIUS, 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(THEME_TILE), 0);
    lv_obj_set_style_bg_color(cell, lv_color_hex(THEME_TRACK), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(cell, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_border_width(cell, 3, 0);
    lv_obj_set_style_border_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(cell, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(cell, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_scrollable(cell, false);
}

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}

// The name is the cell's first label ("fw ui" reports it for the focus), the badge a floating second one
static void add_cell(lv_obj_t *grid, lv_group_t *group, int i) {
    lv_obj_t *cell = lv_obj_create(grid);
    lv_obj_remove_style_all(cell);
    style_cell(cell);
    int32_t w      = (LV_HOR_RES - 2 * THEME_PAD - (MESH_GRID_COLS - 1) * GRID_GAP) / MESH_GRID_COLS;
    int32_t line_h = lv_font_get_line_height(lv_obj_get_style_text_font(cell, LV_PART_MAIN)); // a taller font (#28)
    lv_obj_set_size(cell, w, ICON_PX + ICON_GAP + line_h + 2 * CELL_PAD);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, ICON_GAP, 0);
    meshcore_grid_icon(cell, i, ICON_PX);
    lv_obj_t *name = add_label(cell, mesh_grid_name((mesh_grid_tile_t)i), THEME_TEXT);
    lv_obj_set_width(name, w - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    g.badge[i] = add_label(cell, "", THEME_AMBER);
    lv_obj_set_floating(g.badge[i], true);
    lv_obj_align(g.badge[i], LV_ALIGN_TOP_RIGHT, -14, 10);
    lv_obj_add_event_cb(cell, on_key, LV_EVENT_KEY, (void *)(intptr_t)i);
    if (group) {
        lv_group_add_obj(group, cell);
    }
    g.cell[i] = cell;
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (g.timer) {
        lv_timer_delete(g.timer);
    }
    if (g.status_timer) {
        lv_timer_delete(g.status_timer);
    }
    meshcore_advert_forget(advert_status);
    memset(&g, 0, sizeof(g));
}

void meshcore_grid_build(lv_obj_t *screen, lv_group_t *group) {
    memset(&g, 0, sizeof(g));
    g.screen = screen;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(screen, GRID_GAP, 0);
    lv_obj_set_style_pad_column(screen, GRID_GAP, 0);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    for (int i = 0; i < MESH_GRID_TILES; i++) {
        add_cell(screen, group, i);
    }
    g.status = add_label(screen, "", THEME_TEAL);
    lv_obj_set_width(g.status, LV_PCT(100));
    lv_obj_t *hint = add_label(screen, "Arrows: choose    Enter: open    Esc: home", THEME_AMBER);
    lv_obj_set_floating(hint, true);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    if (group) {
        lv_group_focus_obj(g.cell[MESH_GRID_CHAT]);
    }
    g.status_timer = lv_timer_create(clear_status, STATUS_KEEP_MS, NULL);
    if (g.status_timer) {
        lv_timer_pause(g.status_timer); // runs from the first result on
    }
    g.timer = lv_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL); // the screen is not loaded yet: the badges right away all the same
}
