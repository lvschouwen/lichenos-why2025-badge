// The MeshCore grid's tile icons (#88), drawn rather than font symbols: a label inside a tile would be the first text
// "fw ui" reads for the focused tile, ahead of its name. In a 60x60 view like the app's home icon, on a rounded square
// as Lichen's own app icons.
#include "mesh_grid.h"
#include "meshcore_screens.h"
#include "theme.h"

#include <stdint.h>

typedef struct {
    lv_layer_t *layer;
    int32_t     x, y, size; // the drawing's box: top left and side
} pen_t;

// From the 60x60 view to the screen
static int32_t px(pen_t const *p, int32_t v) {
    return v * p->size / 60;
}

static void arc(pen_t const *p, int32_t cx, int32_t cy, int32_t r, int32_t from, int32_t to, lv_opa_t opa) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color       = lv_color_hex(THEME_AMBER);
    d.opa         = opa;
    d.width       = LV_MAX(2, px(p, 5));
    d.rounded     = 1;
    d.center.x    = p->x + px(p, cx);
    d.center.y    = p->y + px(p, cy);
    d.radius      = (uint16_t)px(p, r);
    d.start_angle = from;
    d.end_angle   = to;
    lv_draw_arc(p->layer, &d);
}

static void disc(pen_t const *p, int32_t cx, int32_t cy, int32_t r, uint32_t color) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius     = LV_RADIUS_CIRCLE;
    d.bg_color   = lv_color_hex(color);
    int32_t   x  = p->x + px(p, cx), y = p->y + px(p, cy), rr = LV_MAX(2, px(p, r));
    lv_area_t a  = {x - rr, y - rr, x + rr - 1, y + rr - 1};
    lv_draw_rect(p->layer, &d, &a);
}

static void line(pen_t const *p, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color       = lv_color_hex(THEME_AMBER);
    d.width       = LV_MAX(2, px(p, 5));
    d.round_start = 1;
    d.round_end   = 1;
    d.p1.x        = p->x + px(p, x1);
    d.p1.y        = p->y + px(p, y1);
    d.p2.x        = p->x + px(p, x2);
    d.p2.y        = p->y + px(p, y2);
    lv_draw_line(p->layer, &d);
}

// A speech bubble with three dots
static void draw_chat(pen_t const *p) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius    = px(p, 10);
    d.bg_color  = lv_color_hex(THEME_AMBER);
    lv_area_t a = {p->x + px(p, 10), p->y + px(p, 14), p->x + px(p, 50), p->y + px(p, 40)};
    lv_draw_rect(p->layer, &d, &a);
    line(p, 18, 38, 14, 48); // the tail
    for (int i = 0; i < 3; i++) {
        disc(p, 20 + 10 * i, 27, 3, THEME_TRACK);
    }
}

// A head and shoulders
static void draw_contacts(pen_t const *p) {
    disc(p, 30, 22, 9, THEME_AMBER);
    arc(p, 30, 50, 15, 180, 360, LV_OPA_COVER);
}

// A mast with a node on top, sending both ways
static void draw_repeaters(pen_t const *p) {
    line(p, 30, 26, 22, 50);
    line(p, 30, 26, 38, 50);
    line(p, 25, 42, 35, 42);
    disc(p, 30, 22, 5, THEME_AMBER);
    arc(p, 30, 22, 13, 130, 230, LV_OPA_COVER);
    arc(p, 30, 22, 13, 310, 50, LV_OPA_COVER);
}

// A node with waves arriving from the right: what came in over the air
static void draw_heard(pen_t const *p) {
    disc(p, 18, 30, 5, THEME_AMBER);
    arc(p, 18, 30, 14, 315, 45, LV_OPA_COVER);
    arc(p, 18, 30, 24, 320, 40, LV_OPA_70);
    arc(p, 18, 30, 34, 325, 35, LV_OPA_40);
}

// A node calling out in every direction
static void draw_advertise(pen_t const *p) {
    disc(p, 30, 30, 6, THEME_AMBER);
    arc(p, 30, 30, 14, 135, 225, LV_OPA_COVER);
    arc(p, 30, 30, 14, 315, 45, LV_OPA_COVER);
    arc(p, 30, 30, 23, 140, 220, LV_OPA_60);
    arc(p, 30, 30, 23, 320, 40, LV_OPA_60);
}

// A gear: a ring with eight teeth
static void draw_settings(pen_t const *p) {
    arc(p, 30, 30, 12, 0, 360, LV_OPA_COVER);
    for (int16_t deg = 0; deg < 360; deg += 45) {
        int32_t s = lv_trigo_sin(deg), c = lv_trigo_cos(deg);
        line(p, 30 + (15 * c >> LV_TRIGO_SHIFT), 30 + (15 * s >> LV_TRIGO_SHIFT), 30 + (21 * c >> LV_TRIGO_SHIFT),
             30 + (21 * s >> LV_TRIGO_SHIFT));
    }
}

static void on_draw(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    pen_t p = {.layer = lv_event_get_layer(e), .x = box.x1, .y = box.y1, .size = lv_area_get_width(&box)};
    switch ((mesh_grid_tile_t)(intptr_t)lv_event_get_user_data(e)) {
        case MESH_GRID_CHAT: draw_chat(&p); break;
        case MESH_GRID_CONTACTS: draw_contacts(&p); break;
        case MESH_GRID_REPEATERS: draw_repeaters(&p); break;
        case MESH_GRID_HEARD: draw_heard(&p); break;
        case MESH_GRID_ADVERTISE: draw_advertise(&p); break;
        case MESH_GRID_SETTINGS: draw_settings(&p); break;
        default: break;
    }
}

void meshcore_grid_icon(lv_obj_t *parent, int tile, int size_px) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size_px, size_px);
    lv_obj_set_style_radius(box, size_px / 4, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(box, false);
    lv_obj_set_clickable(box, false);
    lv_obj_add_event_cb(box, on_draw, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)tile);
}
