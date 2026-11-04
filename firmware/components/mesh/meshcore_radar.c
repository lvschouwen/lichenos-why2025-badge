// The MeshCore app's Contact radar (#171): contacts that advertise a position, plotted by distance and bearing around
// our own manual position (#52), with the same contacts closest first in a list beside the plot. No map tiles. The ring
// is the smallest 1-2-5 step holding the farthest contact. Enter opens the highlighted contact's details (#49).
#include "lichen_mem.h"
#include "meshcore_run.h"
#include "esp_log.h"
#include "fonts.h"
#include "lichen_ui.h"
#include "mesh.h"
#include "mesh_advert_cfg.h"
#include "mesh_format.h"
#include "meshcore_contact.h"
#include "meshcore_list.h"
#include "meshcore_screens.h"
#include "screen_ids.h"
#include "theme.h"

#include <stdio.h>
#include <string.h>

static char const TAG[] = "meshcore";

#define TICK_MS        2000
#define SCREEN_WAIT_MS 20
#define PLOT_PX        270 // the plot's square; the list gets the rest of the width
#define PLOT_R         118 // ring radius, leaving room for the dots at the edge
#define DOT_PX         10
#define DOT_SEL_PX     18

typedef struct {
    radar_contact_t in[RADAR_IN_MAX];
    radar_entry_t   shown[RADAR_MAX];
    uint32_t        keys[RADAR_MAX];
    scroll_item_t   items[RADAR_MAX];
} radar_mem_t;

static radar_mem_t *mem; // PSRAM, while the screen exists
static mc_list_t    list;
static lv_timer_t  *timer;
static lv_obj_t    *screen_obj, *note_label, *plot, *body;
static uint8_t      sel_key[6]; // the highlighted contact, kept when the order changes
static bool         sel_valid;
static uint32_t     shown_sig;
static uint32_t     shown_range_m; // the ring of the rows on screen, for redrawing the plot alone
// What the screen shows now: the empty states are built once, not every tick (that would reset the scroll)
typedef enum { SHOWN_NONE, SHOWN_READING, SHOWN_NOT_RUNNING, SHOWN_NO_POSITION, SHOWN_NO_CONTACTS, SHOWN_RADAR } shown_t;
static shown_t shown_state;

static void settle_selection(size_t n) {
    size_t at = list.sel < n ? list.sel : (n ? n - 1 : 0);
    for (size_t i = 0; sel_valid && i < n; i++) {
        if (memcmp(mem->shown[i].prefix, sel_key, sizeof(sel_key)) == 0) {
            at = i;
            break;
        }
    }
    list.sel  = at;
    sel_valid = n > 0;
    if (sel_valid) {
        memcpy(sel_key, mem->shown[at].prefix, sizeof(sel_key));
    }
}

static lv_obj_t *circle(lv_obj_t *parent, int d, uint32_t color, bool filled) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    if (filled) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    } else {
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    }
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// The ring, its half-way ring, north and us at the centre; then a dot per contact, the highlighted one larger and amber
static void draw_plot(size_t n, uint32_t range_m) {
    lv_obj_clean(plot);
    lv_obj_align(circle(plot, 2 * PLOT_R, THEME_FAINT, false), LV_ALIGN_CENTER, 0, 0);
    lv_obj_align(circle(plot, PLOT_R, THEME_FAINT, false), LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *north = lv_label_create(plot);
    lv_obj_set_style_text_font(north, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_text_color(north, lv_color_hex(THEME_DIM), 0);
    lv_label_set_text(north, "N");
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_align(circle(plot, DOT_PX, THEME_TEAL, true), LV_ALIGN_CENTER, 0, 0); // us
    for (size_t i = n; i-- > 0;) { // the highlighted one last: on top
        if (i == list.sel) {
            continue;
        }
        int x, y;
        radar_plot_xy(mem->shown[i].dist_m, mem->shown[i].bearing_deg, range_m, PLOT_R, &x, &y);
        lv_obj_align(circle(plot, DOT_PX, THEME_TEXT, true), LV_ALIGN_CENTER, x, y);
    }
    if (n) {
        int x, y;
        radar_plot_xy(mem->shown[list.sel].dist_m, mem->shown[list.sel].bearing_deg, range_m, PLOT_R, &x, &y);
        lv_obj_align(circle(plot, DOT_SEL_PX, THEME_AMBER, true), LV_ALIGN_CENTER, x, y);
    }
}

static void add_entry_row(radar_entry_t const *e, bool selected) {
    char dist[16], detail[64];
    radar_format_dist(e->dist_m, dist, sizeof(dist));
    snprintf(detail, sizeof(detail), "%s %s, %s", dist, radar_compass(e->bearing_deg), mesh_type_name(e->type));
    lv_obj_t *row = mc_list_row(&list, selected);
    // full width, so DOT cuts a long name with "..." (a content-sized label never cuts; #133 sweep hit)
    lv_obj_set_width(mc_list_line(row, e->name[0] ? e->name : "(no name)", THEME_TEXT), LV_PCT(100));
    lv_obj_set_width(mc_list_line(row, detail, THEME_DIM), LV_PCT(100));
}

// An empty note is hidden, so it does not leave a gap under the heading
static void set_note(char const *text) {
    if (strcmp(lv_label_get_text(note_label), text) != 0) {
        lv_label_set_text(note_label, text);
    }
    if (text[0]) {
        lv_obj_remove_flag(note_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(note_label, LV_OBJ_FLAG_HIDDEN);
    }
}

// The list's place says why there is nothing to plot; once per state
static void show_empty(shown_t state, char const *why) {
    if (shown_state == state) {
        return;
    }
    lv_obj_clean(list.list);
    list.n = 0;
    lv_obj_clean(plot);
    mc_list_label(list.list, why, THEME_DIM);
    set_note("");
    shown_state = state;
    shown_sig   = UINT32_MAX;
}

static void rebuild(size_t n, uint32_t range_m, double own_lat, double own_lon) {
    mc_scroll_t s = mc_list_save(&list);
    lv_obj_clean(list.list);
    list.n = n;
    settle_selection(n);
    for (size_t i = 0; i < n; i++) {
        list.keys[i] = scroll_key_hash(mem->shown[i].prefix, sizeof(mem->shown[i].prefix), 0);
        add_entry_row(&mem->shown[i], i == list.sel);
    }
    mc_list_restore(&list, s);
    char ring[16], where[40], text[112];
    radar_format_dist(range_m, ring, sizeof(ring));
    advcfg_coord_text(own_lat, own_lon, where, sizeof(where));
    snprintf(text, sizeof(text), "Outer ring %s, north up. Around %s.", ring, where);
    set_note(text);
    shown_range_m = range_m;
    shown_state   = SHOWN_RADAR;
    draw_plot(n, range_m);
}

static uint32_t signature(size_t n, double lat, double lon) {
    uint32_t h = scroll_key_hash(&lat, sizeof(lat), 0);
    h          = scroll_key_hash(&lon, sizeof(lon), h);
    for (size_t i = 0; i < n; i++) {
        h = scroll_key_hash(mem->shown[i].prefix, sizeof(mem->shown[i].prefix), h);
        h = scroll_key_hash(&mem->shown[i].dist_m, sizeof(mem->shown[i].dist_m), h);
        h = scroll_key_hash(&mem->shown[i].bearing_deg, sizeof(mem->shown[i].bearing_deg), h);
        h = scroll_key_hash(mem->shown[i].name, strnlen(mem->shown[i].name, sizeof(mem->shown[i].name)), h);
    }
    return h;
}

static void refresh(lv_timer_t *t) {
    if (t && lv_screen_active() != screen_obj) {
        return; // the details are open on top
    }
    mesh_advert_cfg_t cfg;
    if (!mesh_running() || !mesh_get_advert_cfg(&cfg)) {
        show_empty(SHOWN_NOT_RUNNING, "MeshCore is not running: the radar comes back when it does.");
        return;
    }
    if (!advcfg_location_set(cfg.lat, cfg.lon)) {
        show_empty(SHOWN_NO_POSITION, "Your own position is not set. Enter it under MeshCore settings > Advert & contacts "
                                      "(Latitude, Longitude): the radar then shows contacts around it.");
        return;
    }
    size_t got = mesh_contacts_positions(mem->in, RADAR_IN_MAX, SCREEN_WAIT_MS);
    if (got == MESH_BUSY) {
        if (shown_state == SHOWN_NONE) {
            show_empty(SHOWN_READING, "Reading contacts..."); // first open only, never a blank screen (R10)
        }
        return; // MeshCore is busy: the screen stays as it is until the next tick
    }
    size_t n = radar_build(cfg.lat, cfg.lon, mem->in, got, mem->shown, RADAR_MAX);
    if (!n) {
        show_empty(SHOWN_NO_CONTACTS, "No contact has shared a position yet: they appear when a node adverts with its location.");
        return;
    }
    uint32_t sig = signature(n, cfg.lat, cfg.lon);
    if (sig != shown_sig) {
        rebuild(n, radar_range_m(mem->shown[n - 1].dist_m), cfg.lat, cfg.lon);
        shown_sig = sig;
    }
}

static void on_key(lv_event_t *e) {
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || key == LV_KEY_DOWN) {
        if (list.n && mc_list_move(&list, key == LV_KEY_UP ? -1 : 1)) {
            memcpy(sel_key, mem->shown[list.sel].prefix, sizeof(sel_key));
            draw_plot(list.n, shown_range_m); // only the highlight moves: no mesh read, no rebuild
        }
    } else if (key == LV_KEY_ENTER && list.n) {
        radar_entry_t const *r = &mem->shown[list.sel];
        mesh_contact_t       c = {.type = r->type};
        memcpy(c.pubkey, r->prefix, sizeof(c.pubkey));
        snprintf(c.name, sizeof(c.name), "%s", r->name);
        meshcore_contact_open(&c);
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (timer) {
        lv_timer_delete(timer);
        timer = NULL;
    }
    lichen_free(meshcore_self(), mem);
    mem        = NULL;
    screen_obj = note_label = plot = body = NULL;
    list       = (mc_list_t){0};
}

// The list and the plot side by side in one row under the note
static void build_body(lv_obj_t *screen) {
    body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 12, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    plot = lv_obj_create(body);
    lv_obj_remove_style_all(plot);
    lv_obj_set_size(plot, PLOT_PX, PLOT_PX);
    lv_obj_remove_flag(plot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_parent(list.list, body);
    lv_obj_set_height(list.list, LV_PCT(100));
    lv_obj_set_flex_grow(list.list, 1);
    lv_obj_set_width(list.list, 0); // the grow decides the width
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    screen_obj       = screen;
    lv_obj_add_event_cb(screen, on_delete, LV_EVENT_DELETE, NULL);
    mem = lichen_alloc(meshcore_self(), sizeof(*mem), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
    if (!mem) {
        ESP_LOGE(TAG, "no memory for the contact radar");
        mc_list_label(screen, "Not enough memory for this page.", THEME_DIM);
        return screen;
    }
    list      = (mc_list_t){.keys = mem->keys, .items = mem->items, .cap = RADAR_MAX};
    list.list = mc_list_screen(screen, "", "", "Enter", "details");
    // R1: the kind small and dim over the name, in place of the list screen's one-line heading
    lv_obj_delete(lv_obj_get_child(screen, 0));
    lv_obj_move_to_index(lichen_heading(screen, "MeshCore", "Contact radar"), 0);
    note_label = lv_obj_get_child(screen, 1);
    build_body(screen);
    lv_obj_add_event_cb(list.list, on_key, LV_EVENT_KEY, NULL);
    shown_sig   = UINT32_MAX;
    shown_state = SHOWN_NONE;
    sel_valid   = false;
    timer     = lv_timer_create(refresh, TICK_MS, NULL);
    refresh(NULL);
    return screen;
}

void meshcore_radar_open(void *arg) {
    (void)arg;
    shell_open(build, SCREEN_MC_RADAR);
}
