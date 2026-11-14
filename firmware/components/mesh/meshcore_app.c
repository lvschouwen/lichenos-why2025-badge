// MeshCore as a Lichen app (#67): its home tile (unread, nodes, the latest message), its screen (the app's own grid of
// tiles, #88: meshcore_grid.c, with the lists in meshcore_chats.c and meshcore_contacts.c), its about section under
// Settings and its background part (the mesh task, started from main.c whatever screen is open). Esc on the grid is
// the shell's. Everything with an lv_obj_t runs in the LVGL task; it reads MeshCore only through mesh.h copies.
#include "bench_scope.h"
#include "esp_log.h"
#include "screen_ids.h"
#include "lichen_app.h"
#include "lora_app.h"
#include "mc_first.h"
#include "mesh_advert_cfg.h"
#include "menu.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_radio_line.h"
#include "meshcore_app_record.h"
#include "meshcore_run.h"
#include "meshcore_screens.h"
#include "meshcore_settings.h"
#include "lichen_settings.h"
#include "lichen_ui.h"
#include "theme.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

static lichen_app_t const *self(void); // the app table entry, defined with the rest of the table at the end

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
    // Not running, starting, stopping (#103): "Not running" / "Enter to start"
    if (mesh_run_tile_text(meshcore_run_get(), lora_up(), l1, c1, l2, c2) || !mesh_running()) {
        return true;
    }
    mesh_tile_t t;
    mesh_tile_snapshot(&t);
    mesh_tile_text(&t, l1, c1, l2, c2);
    return true;
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
    ESP_LOGI(TAG, "first-open questions left with Esc or not shown: asked again next open");
    meshcore_ask_cancel();
    first_screen = NULL;
    shell_home();
}

#define FIRST_WAIT_MS 300

static lv_obj_t *first_wait; // "Starting MeshCore..." while an on-demand start runs before the questions (#103)

static void first_wait_due(lv_timer_t *timer) {
    (void)timer;
    first_step(NULL); // one-shot: LVGL deletes the timer after this call
}

// The questions need MeshCore running (#103: it may have just been asked to start): true when it runs; else a retry
// is set while it is starting, or the grid takes over (saying why) when the start failed
static bool first_needs_running(void) {
    if (mesh_running()) {
        if (first_wait) {
            lv_obj_delete(first_wait);
            first_wait = NULL;
        }
        return true;
    }
    if (mesh_run_busy(meshcore_run_get())) {
        lv_timer_t *t = lv_timer_create(first_wait_due, FIRST_WAIT_MS, NULL);
        if (t) {
            lv_timer_set_repeat_count(t, 1);
        }
        return false;
    }
    ESP_LOGW(TAG, "MeshCore did not start: first-open questions asked again next open");
    lv_obj_t   *screen = first_screen;
    lv_group_t *group  = lv_obj_get_group(screen);
    first_screen       = NULL;
    if (first_wait) {
        lv_obj_delete(first_wait);
        first_wait = NULL;
    }
    if (group) {
        lv_group_remove_obj(screen);
    }
    meshcore_grid_build(screen, group);
    return false;
}

static void first_step(void *arg) {
    (void)arg;
    if (!first_screen || lv_screen_active() != first_screen) {
        return; // a question is open (or the screen is gone): it calls us again when it closes
    }
    if (!first_needs_running()) {
        return;
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
            bool saved = app_settings_set_u8(meshcore_ctx(), "mc_name_set", 1);
            ESP_LOGI(TAG, "first-open questions answered (%s)", saved ? "saved" : "not saved");
            lv_obj_t   *screen = first_screen;
            lv_group_t *group  = lv_obj_get_group(screen);
            first_screen       = NULL;
            if (group) {
                lv_group_remove_obj(screen); // keys go to the tiles, Tab must not land on the bare screen
            }
            meshcore_grid_build(screen, group);
            break;
        }
    }
}

// LOADED comes while the shell is still switching screens: decide a moment later, after the closing question's own done
static void on_first_loaded(lv_event_t *e) {
    (void)e;
    lichen_ui_call(first_step, NULL);
}

static void on_first_delete(lv_event_t *e) {
    (void)e;
    meshcore_ask_cancel();
    first_screen = NULL;
    first_wait   = NULL; // a child of the screen: gone with it
}

static void open(lv_obj_t *screen) {
    shell_on_esc(meshcore_grid_esc, NULL); // arranging the tiles is a state of the grid, not a screen (#189)
    // "Enter to start" (#103): a stopped MeshCore starts on the job task; the grid says so until it runs
    mesh_run_t run = meshcore_run_get();
    if (run.state == MESH_RUN_STOPPED && run.why != MESH_WHY_UPDATE && lora_up()) {
        char const *why = meshcore_run_request(true); // refused for an injected key (#400): it logged that itself
        if (!why || strcmp(why, MESH_RUN_HAND_ONLY_REFUSAL) != 0) {
            ESP_LOGI(TAG, "opened while stopped: start %s", why ? why : "asked");
        }
    }
    bool starting = meshcore_run_get().state == MESH_RUN_STARTING;
    if ((mesh_running() || starting) && app_settings_get_u8(meshcore_ctx(), "mc_name_set", 0) == 0) {
        ESP_LOGI(TAG, "first open: asking name and region%s", starting ? " once MeshCore runs" : "");
        first        = (mc_first_t){0};
        first_screen = screen;
        first_wait   = NULL;
        if (starting) {
            first_wait = lv_label_create(screen);
            lv_label_set_text(first_wait, "Starting MeshCore...");
            lv_obj_set_style_text_color(first_wait, lv_color_hex(THEME_AMBER), 0);
        }
        lv_obj_add_event_cb(screen, on_first_loaded, LV_EVENT_SCREEN_LOADED, NULL);
        lv_obj_add_event_cb(screen, on_first_delete, LV_EVENT_DELETE, NULL);
        return;
    }
    meshcore_grid_build(screen, lv_group_get_default()); // the screen's own (shell_open)
}

// ---- Settings > MeshCore: about ----------------------------------------------------------------------------------

static lv_obj_t *about_radio; // the about rows' values that a radio change makes stale, NULL once the page is gone
static lv_obj_t *about_power;

// Width of the key column: the widest key in the current font plus a gap, so a taller font cannot make a key touch
// its value (#28)
static int32_t key_col_w(lv_obj_t *obj, char const *const *keys, size_t n) {
    lv_font_t const *font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
    int32_t          w    = 0;
    for (size_t i = 0; i < n; i++) {
        lv_point_t size;
        lv_text_get_size(&size, keys[i], font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        w = size.x > w ? size.x : w;
    }
    return w + 24;
}

static lv_obj_t *add_row(lv_obj_t *page, char const *key, char const *value) {
    static char const *const KEYS[] = {"Node ID", "Radio", "TX power", "Version"};
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    lv_obj_t *k = lv_label_create(row);
    lv_obj_set_width(k, key_col_w(page, KEYS, sizeof(KEYS) / sizeof(KEYS[0])));
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(k, key);
    lv_obj_t *v = lichen_label_wrap(row, 0); // a value from the node can be long: it wraps, never clips (R2)
    lv_obj_set_flex_grow(v, 1);
    lv_label_set_text(v, value);
    return v;
}

// What the radio runs now (#335): the C6's applied lora.* from the registry cache, not MeshCore's NodePrefs (those are
// what it asks for; its radio pages show both). Never waits: safe on the LVGL task.
static void radio_texts(char *text, size_t cap, char *power, size_t power_cap) {
    mesh_applied_t applied;
    mesh_applied_read(&applied);
    mesh_applied_radio(&applied, text, cap);
    mesh_applied_power(&applied, power, power_cap);
}

static void set_if_changed(lv_obj_t *label, char const *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) { // the rows wrap: an unchanged rewrite would relayout the page
        lv_label_set_text(label, text);
    }
}

void meshcore_about_refresh(void) {
    if (about_radio && about_power) {
        char text[96], power[48];
        radio_texts(text, sizeof(text), power, sizeof(power));
        set_if_changed(about_radio, text);
        set_if_changed(about_power, power);
    }
}

// The registry polls the C6 about once a second: the rows follow it while the page is up
static void about_tick(lv_timer_t *timer) {
    (void)timer;
    meshcore_about_refresh();
}

static void on_about_delete(lv_event_t *e) {
    lv_timer_t *timer = lv_event_get_user_data(e);
    if (timer) {
        lv_timer_delete(timer);
    }
    if (lv_event_get_target(e) == about_radio) { // a later About page owns them by now: keep its rows
        about_radio = NULL;
        about_power = NULL;
    }
}

static void about(lv_obj_t *page) {
    menu_note(page, "About");
    char text[96];
    if (!mesh_node_id(text, sizeof(text))) {
        strlcpy(text, "-", sizeof(text)); // not running
    }
    add_row(page, "Node ID", text);

    char power[48];
    radio_texts(text, sizeof(text), power, sizeof(power));
    about_radio = add_row(page, "Radio", text);
    about_power = add_row(page, "TX power", power);
    lv_obj_add_event_cb(about_radio, on_about_delete, LV_EVENT_DELETE, lv_timer_create(about_tick, 1000, NULL));
    snprintf(text, sizeof(text), "MeshCore %s", mesh_firmware_version());
    add_row(page, "Version", text);
}

// ---- the grid's Settings tile: the same page as Settings > MeshCore (app_settings.c builds it alike) -------------

static lv_obj_t *build_settings(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create_named(screen, "App settings", self()->name);
    meshcore_settings(menu);
    about(menu);
    return screen;
}

void meshcore_app_settings_open(void) {
    shell_open(build_settings, SCREEN_SETTINGS);
}

// ---- the background part -----------------------------------------------------------------------------------------

static bool start_background(void) {
    return meshcore_run_boot(); // start at boot on: started here, as before; off: only the console commands (#103)
}

bool meshcore_setup_done(void) {
    return app_settings_get_u8(meshcore_ctx(), "mc_name_set", 0) != 0;
}

// "fw setup reset meshcore" / "done meshcore": only the questions' flag; the name and the radio stay
static void first_run_set(bool done) {
    bool ok = app_settings_set_u8(meshcore_ctx(), "mc_name_set", done ? 1 : 0);
    ESP_LOGI(
        TAG,
        "first-open questions marked %s (%s)",
        done ? "answered" : "to ask again",
        ok ? "saved" : "not saved"
    );
}

// The mesh saves and stops; the OS stops the radio afterwards, only when this says true (#84 T7)
static bool stop_for_update(void) {
    bool stopped = meshcore_run_stop_for_update();
    if (!stopped) {
        ESP_LOGE(TAG, "MeshCore still running");
    }
    return stopped;
}

// The Messages key (▲, #79): the Chat tile's conversation list, unless open() is asking the first-run questions
static void open_messages(void) {
    if (!first_screen) {
        meshcore_chats_open();
    }
}

static lichen_app_t const meshcore_app = {
    .name             = "MeshCore",
    .id               = MESHCORE_APP_ID,
    .draw_icon        = icon,
    .tile_text        = tile_text,
    .open             = open,
    .settings         = meshcore_settings,
    .about            = about,
    .start_background = start_background,
    .safe_mode_exempt = true, // #232: the main app's background is never switched off by safe mode
    .stop_for_update  = stop_for_update,
    .first_run_set    = first_run_set,
    .open_messages    = open_messages,
    .open_notification    = meshcore_open_notification,
    .notify_settings_open = meshcore_notify_settings_open,
    .alias            = MESHCORE_ALIAS,
    .keep_keys        = meshcore_keep_keys,
    .storage_root     = MESHCORE_STORAGE_ROOT,
    .storage_secret   = MESHCORE_STORAGE_SECRET,
    .backup_keys      = meshcore_backup_keys,
    .backup_key_count = MESHCORE_BACKUP_KEYS,
    .secret_prefix    = MESHCORE_SECRET_PREFIX,
    // #229: budgets are the static floor (statics, mesh and job stacks); the heap part is measured on the bench
    .caps = CAP_LORA_TX | CAP_LORA_RX | CAP_SD | CAP_SECRETS | CAP_CONSOLE | CAP_RESTART | CAP_BACKGROUND | CAP_NOTIFY,
    .internal_ram_kib = 48,
    .psram_kib        = 24,
};

static lichen_app_t const *self(void) {
    return &meshcore_app;
}

// bench_enter_starts (#377): Enter on the grid (any tile: the check does not read which) or on MeshCore's home tile
// would start a stopped MeshCore (open() and the grid's start_instead), and a start may send a due periodic advert
static bool remote_enter_starts(int top_screen, char const *focus) {
    bool home_tile = top_screen == 0 && focus && strcmp(focus, meshcore_app.name) == 0;
    return mesh_run_enter_starts(meshcore_run_get(), home_tile || meshcore_grid_on_top());
}

void meshcore_app_register(void) {
    bench_starts_bind(remote_enter_starts); // from boot on, before home shows its tile
    meshcore_run_init(lichen_register_app(&meshcore_app));
}
