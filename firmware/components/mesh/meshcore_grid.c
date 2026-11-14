// The MeshCore app's home grid (#88): eight tiles in three rows in Lichen home's grid look (home.c add_grid_cell) and key rules
// (mesh_grid_move = home_move). Each tile is a focusable object in the screen's group, so "fw ui" names the focused
// tile; the arrows move the focus, Enter opens the tile's screen on the shell's stack (Esc there comes back here) and
// Esc here is the shell's: back to Lichen home. Badges refresh once a second from copies (mesh.h), never waiting on
// MeshCore for more than a tick.
//
// Arrange mode (#189), as home's: g.cell[], g.badge[] and the tile ids stay indexed by tile; g.order[slot] / g.slot_of[tile]
// are only where a tile is shown, and the arrows move in slots (mesh_grid_move). Settings > Arrange tiles starts it:
// Enter picks the focused tile up, the arrows move it (insert and shift, the cells are reordered in place), Enter drops
// and saves, Esc puts it back or ends the mode. The order is saved only on a real move (mesh_order.h).
#include "esp_log.h"
#include "screen_ids.h"
#include "lichen_clock.h"
#include "lichen_ui.h"
#include "lora_app.h"
#include "mesh.h"
#include "mesh_chat.h"
#include "mesh_format.h"
#include "mesh_grid.h"
#include "mesh_order.h"
#include "lichen_notify.h"
#include "tile_order.h"
#include "meshcore_run.h"
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

#define BADGE_DOT_PX   8 // before a countdown badge (#193): the time until, not since
#define BADGE_EDGE     6 // the badge off the cell's inner right edge
#define TILE_RADIUS    20
#define REFRESH_MS     1000
#define SCREEN_WAIT_MS 20
#define ADVERT_ASK_S   15 // the confirm cancels by itself after this: nothing goes on air unasked
#define STATUS_KEEP_MS 10000 // the Advertise result stays this long under the grid

static struct {
    lv_obj_t          *screen;
    lv_obj_t          *cell[MESH_GRID_TILES];
    lv_obj_t          *badge[MESH_GRID_TILES];
    lv_obj_t          *badge_dot[MESH_GRID_TILES]; // shown before a countdown (#193)
    lv_obj_t          *status; // the Advertise result; NULL once the grid is gone
    lv_obj_t          *run;    // "MeshCore is not running. Enter starts it." over the tiles, hidden while it runs
    lv_timer_t        *timer;
    lv_timer_t        *status_timer; // clears the status line
    lv_group_t        *group;
    int                order[MESH_GRID_TILES];   // order[slot] = the tile shown there
    int                slot_of[MESH_GRID_TILES];
    int                picked[MESH_GRID_TILES];  // the order when the tile was picked up: Esc puts it back
    bool               arranging;
    bool               holding;
    lv_obj_t          *moving; // the amber "Moving" label on the held tile

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
        c->contacts    = st.total - st.repeaters; // the Contacts list: the people only (#101)
        c->repeaters   = st.repeaters;
        c->heard_any   = st.heard_any;
        c->heard_age_s = st.newest_age_s;
    }
}

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != g.screen) {
        return; // a tile's screen is on top: no mesh lock for badges nobody sees
    }
    mesh_grid_counts_t *c   = &g.counts;
    char const         *run = mesh_run_grid_text(meshcore_run_get(), lora_up());
    if (g.arranging && run[0]) {
        run = "Arranging tiles"; // Enter picks up now, it does not start MeshCore
    }
    set_text(g.run, run);
    if (g.run && lv_obj_is_hidden(g.run) != (run[0] == '\0')) {
        lv_obj_set_hidden(g.run, run[0] == '\0'); // only on a change, as set_text
    }
    if (mesh_running()) {
        mesh_tile_t tile;
        mesh_tile_snapshot(&tile); // never waits: the home tile's own copy
        c->unread       = tile.unread_total;
        c->advert_sent  = tile.advert_sent;
        c->advert_age_s = tile.advert_age_s;
        c->advert_scheduled = tile.advert_scheduled;
        c->advert_next_s    = tile.advert_next_s;
        take_contact_counts(c);
    } else {
        *c = (mesh_grid_counts_t){0};
    }
    for (int i = 0; i < MESH_GRID_TILES; i++) {
        char text[MESH_GRID_BADGE_MAX];
        bool countdown = mesh_grid_badge((mesh_grid_tile_t)i, c, text, sizeof(text));
        set_text(g.badge[i], text);
        if (g.badge_dot[i] && lv_obj_is_hidden(g.badge_dot[i]) == countdown) {
            lv_obj_set_hidden(g.badge_dot[i], !countdown);
        }
    }
}

// ---- Advertise: confirmed first, the result under the grid ----------------------------------------------------------

// The result line takes no room while empty: a fourth flow row would push the tiles into the hint
static void set_status(char const *text) {
    if (!g.status) {
        return;
    }
    set_text(g.status, text);
    if (lv_obj_is_hidden(g.status) != (text[0] == '\0')) {
        lv_obj_set_hidden(g.status, text[0] == '\0');
    }
}

static void clear_status(lv_timer_t *t) {
    (void)t;
    set_status("");
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
    set_status(line);
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
        show_status("Not sent");
        return;
    }
    ESP_LOGI(TAG, "advert confirmed on the grid");
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
    mesh_advert_cfg_t cfg = {0};
    mesh_get_advert_cfg(&cfg);
    shell_dialog_t const d = {
        .title        = cfg.flood_manual ? "Send a flood advert now? The whole mesh hears it."
                                         : "Send an advert now? Nodes in direct range hear it.",
        .seconds       = ADVERT_ASK_S,
        .careful       = true, // a held Enter never sends it, the countdown ends in a cancel
        .confirm_label = "send",
    };
    if (!shell_dialog_countdown(&d, advert_answer)) {
        show_status(mc_busy_note());
    }
}

// ---- keys --------------------------------------------------------------------------------------------------------

// A stopped MeshCore (#103): Enter on any tile but Settings starts it, as the line above the tiles says; true when
// that is what Enter did
static bool start_instead(mesh_grid_tile_t tile) {
    mesh_run_t r = meshcore_run_get();
    bool final = r.state == MESH_RUN_STOPPED && r.why == MESH_WHY_UPDATE;
    if (tile == MESH_GRID_SETTINGS || r.state == MESH_RUN_RUNNING || final) {
        return false;
    }
    if (r.state == MESH_RUN_STOPPED) {
        char const *why = meshcore_run_request(true); // refused for an injected key (#400): it logged that itself
        if (!why || strcmp(why, MESH_RUN_HAND_ONLY_REFUSAL) != 0) {
            ESP_LOGI(TAG, "start from the grid: %s", why ? why : "asked");
        }
    }
    refresh(NULL); // "Starting MeshCore..." at once
    return true;   // starting or stopping: the tiles wait for it
}

bool meshcore_grid_on_top(void) {
    return g.screen && shell_stack_screen(0) == g.screen;
}

static void open_tile(void *arg) {
    clear_status(NULL); // an old result does not belong to what comes next
    if (!g.screen || start_instead((mesh_grid_tile_t)(intptr_t)arg)) {
        return;
    }
    switch ((mesh_grid_tile_t)(intptr_t)arg) {
        case MESH_GRID_CHAT: meshcore_chats_open(); break;
        case MESH_GRID_CONTACTS: meshcore_contacts_open(MESH_VIEW_CONTACTS); break;
        case MESH_GRID_REPEATERS: meshcore_contacts_open(MESH_VIEW_REPEATERS); break;
        case MESH_GRID_HEARD: meshcore_contacts_open(MESH_VIEW_HEARD); break;
        case MESH_GRID_NEARBY: meshcore_discover_open(NULL); break;
        case MESH_GRID_RADAR: meshcore_radar_open(NULL); break;
        case MESH_GRID_ADVERTISE: ask_advert(); break;
        case MESH_GRID_SETTINGS: meshcore_app_settings_open(); break;
        default: break;
    }
}

// ---- arrange mode (#189) -----------------------------------------------------------------------------------------

static void set_hint(void) {
    if (!g.screen) {
        return;
    }
    if (g.holding) {
        shell_page_hint(g.screen, "Arrows", "move", "Enter", "drop", "Esc", "cancel", NULL);
    } else if (g.arranging) {
        shell_page_hint(g.screen, "Enter", "pick up", "Esc", "done", NULL);
    } else {
        shell_page_hint(g.screen, "Enter", "open", NULL); // dim (R8), the same line as every page
    }
}

// The cells are the screen's children after the run line, in display order; the Advertise result line and the hint
// stay behind them. The key group follows, so Tab visits them in the order they are shown.
static void place_cells(int focus) {
    for (int slot = 0; slot < MESH_GRID_TILES; slot++) {
        lv_obj_move_to_index(g.cell[g.order[slot]], 1 + slot);
    }
    if (g.group) {
        for (int slot = 0; slot < MESH_GRID_TILES; slot++) {
            lv_group_remove_obj(g.cell[g.order[slot]]);
        }
        for (int slot = 0; slot < MESH_GRID_TILES; slot++) {
            lv_group_add_obj(g.group, g.cell[g.order[slot]]);
        }
        lv_group_focus_obj(g.cell[focus]);
    }
    lv_obj_update_layout(g.screen);
}

static void show_moving(int tile, bool on) {
    if (g.moving) {
        lv_obj_delete(g.moving);
        g.moving = NULL;
    }
    if (!on) {
        return;
    }
    lv_obj_t *label = lichen_label_line(g.cell[tile], LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, LV_PCT(100), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_AMBER), 0);
    lv_label_set_text(label, "Moving");
    // Floating in the cell's bottom pad, under the name: in the flow it would push the icon up, away from its badge.
    // The pad lies outside the content area the alignment uses, so its height is added back.
    int32_t pad = lv_obj_get_style_pad_bottom(g.cell[tile], LV_PART_MAIN);
    lv_obj_set_floating(label, true);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, pad - TILE_GRID_MOVING_EDGE);
    g.moving = label;
}

static int focused_tile(void) {
    lv_obj_t *f = g.group ? lv_group_get_focused(g.group) : NULL;
    for (int i = 0; i < MESH_GRID_TILES; i++) {
        if (g.cell[i] == f) {
            return i;
        }
    }
    return g.order[0];
}

static void cancel_hold(void) {
    int tile = focused_tile();
    memcpy(g.order, g.picked, sizeof(g.order));
    tile_order_slots(g.order, MESH_GRID_TILES, g.slot_of);
    g.holding = false;
    show_moving(tile, false);
    place_cells(tile);
    set_hint();
}

static void drop(void) {
    int  tile  = focused_tile();
    bool moved = memcmp(g.picked, g.order, sizeof(g.order)) != 0;
    // Only a real move reaches the store; a failed save must not look saved: the tile goes back
    if (moved && !mesh_order_save(g.order)) {
        cancel_hold();
        lichen_notify(meshcore_ctx(), LN_WARN, "Tile order not saved",
                      "The tile went back to its place. Try again; if it keeps failing, restart the badge.", 0);
        return;
    }
    g.holding = false;
    show_moving(tile, false);
    set_hint();
}

static void pick_up(void) {
    memcpy(g.picked, g.order, sizeof(g.picked));
    g.holding = true;
    show_moving(focused_tile(), true);
    set_hint();
}

static void arrange_key(int at, uint32_t key) {
    int from = g.slot_of[at];
    int to   = mesh_grid_move(MESH_GRID_TILES, from, key);
    if (to == from) {
        return;
    }
    if (g.holding) {
        tile_order_move(g.order, MESH_GRID_TILES, from, to);
        tile_order_slots(g.order, MESH_GRID_TILES, g.slot_of);
        place_cells(at); // re-focus the moved cell
    } else {
        lv_group_focus_obj(g.cell[g.order[to]]);
    }
    lv_obj_scroll_to_view(g.cell[g.holding ? at : g.order[to]], LV_ANIM_OFF);
}

// Settings > Arrange tiles: arrange mode on (a held tile goes back when it ends)
static void arrange_set(bool on) {
    if (!g.screen || on == g.arranging) {
        return;
    }
    if (g.holding) {
        cancel_hold();
    }
    g.arranging = on;
    set_hint();
    refresh(NULL); // the run line says "Arranging tiles" (or its own text again) at once
}

// The shell's Esc hook (shell_on_esc, set in meshcore_grid_open): a held tile goes back, else arranging ends; true when
// that used the Esc
bool meshcore_grid_esc(void *arg) {
    (void)arg;
    if (!g.screen || !g.arranging) {
        return false;
    }
    if (g.holding) {
        cancel_hold();
    } else {
        arrange_set(false);
    }
    return true;
}

static void arrange_begin(void *arg) {
    (void)arg;
    arrange_set(true);
}

bool meshcore_grid_arrange_possible(void) {
    return g.screen != NULL;
}

bool meshcore_grid_arrange_start(void) {
    // Back to the grid under Settings, then arranging; the grid is under the top when Settings was opened from its tile
    return g.screen && shell_back_to(g.screen, arrange_begin, NULL);
}

// The saved order changed behind the grid's back (reset): show it, keeping the focus
void meshcore_grid_reload(void) {
    if (!g.screen) {
        return;
    }
    if (g.holding) {
        cancel_hold();
    }
    int tile = focused_tile();
    mesh_order_load(g.order);
    tile_order_slots(g.order, MESH_GRID_TILES, g.slot_of);
    place_cells(tile);
}

static void on_key(lv_event_t *e) {
    int      at  = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER && g.arranging) {
        if (g.holding) {
            drop();
        } else {
            pick_up(); // never opens a tile while arranging
        }
        return;
    }
    if (key == LV_KEY_ENTER) {
        // Outside the indev's key handling: opening swaps the indev's group (as home.c does)
        lichen_ui_call(open_tile, (void *)(intptr_t)at);
        return;
    }
    if (g.arranging) {
        arrange_key(at, key);
        return;
    }
    int from = g.slot_of[at];
    int to   = mesh_grid_move(MESH_GRID_TILES, from, key);
    if (to != from) {
        lv_group_focus_obj(g.cell[g.order[to]]);
        lv_obj_scroll_to_view(g.cell[g.order[to]], LV_ANIM_OFF);
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
    lv_obj_set_style_border_width(cell, TILE_GRID_BORDER_W, 0);
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
    // The same cell as home's grid (#193), sized from the font (#28); the bottom pad holds arrange mode's "Moving"
    int32_t     line_h = lv_font_get_line_height(lv_obj_get_style_text_font(cell, LV_PART_MAIN));
    tile_cell_t size   = tile_cell_size(LV_HOR_RES - 2 * THEME_PAD, MESH_GRID_COLS, line_h);
    lv_obj_set_style_pad_top(cell, TILE_GRID_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(cell, size.pad_bottom, 0);
    lv_obj_set_size(cell, size.w, size.h);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, TILE_GRID_ICON_GAP, 0);
    meshcore_grid_icon(cell, i, TILE_GRID_ICON_PX);
    lv_obj_t *name = add_label(cell, mesh_grid_name((mesh_grid_tile_t)i), THEME_TEXT);
    lv_obj_set_width(name, size.w - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    // The badge in the top right corner, beside the icon: a dot (a countdown, #193) and the text, in a row that grows
    // to the left, so the text keeps its place whether the dot shows or not
    lv_obj_t *row = lv_obj_create(cell);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    lv_obj_set_floating(row, true);
    // The cell's padding is outside the content area the alignment uses: back to the inner edge, then BADGE_EDGE in
    lv_obj_align(row, LV_ALIGN_TOP_RIGHT, -BADGE_EDGE, -TILE_GRID_PAD_TOP + BADGE_EDGE);
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, BADGE_DOT_PX, BADGE_DOT_PX);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(THEME_TEAL), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_hidden(dot, true);
    g.badge_dot[i] = dot;
    g.badge[i]     = add_label(row, "", THEME_AMBER);
    lichen_ui_event_add(cell, on_key, LV_EVENT_KEY, (void *)(intptr_t)i);
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
    lv_obj_set_style_pad_row(screen, TILE_GRID_ROW_GAP, 0);
    lv_obj_set_style_pad_column(screen, TILE_GRID_COL_GAP, 0);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(screen, 0, LV_STATE_FOCUS_KEY);
    g.run = add_label(screen, "", THEME_AMBER);
    lv_obj_set_width(g.run, LV_PCT(100));
    lv_obj_set_hidden(g.run, true); // refresh shows it when MeshCore does not run
    g.group = group;
    mesh_order_load(g.order); // the first order unless the person arranged them; nothing is written here
    tile_order_slots(g.order, MESH_GRID_TILES, g.slot_of);
    for (int slot = 0; slot < MESH_GRID_TILES; slot++) {
        add_cell(screen, group, g.order[slot]); // children and group in display order
    }
    g.status = add_label(screen, "", THEME_TEAL);
    lv_obj_set_width(g.status, LV_PCT(100));
    lv_obj_set_hidden(g.status, true); // set_status shows it with a result
    set_hint();
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    if (group) {
        lv_group_focus_obj(g.cell[g.order[0]]);
    }
    g.status_timer = lichen_ui_timer_create(clear_status, STATUS_KEEP_MS, NULL);
    if (g.status_timer) {
        lv_timer_pause(g.status_timer); // runs from the first result on
    }
    g.timer = lichen_ui_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL); // the screen is not loaded yet: the badges right away all the same
}
