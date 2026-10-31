#include "logo.h"

#include "theme.h"

// lichen.html's <g id="symbio"> in its 120x120 viewBox, values x10 so the 2.5 px strokes and 4.5 px radii survive
typedef struct {
    int16_t x1, y1, x2, y2;
} seg_t;

typedef struct {
    int16_t  x, y, r;
    uint32_t color;
} node_t;

static seg_t const lines[] = {
    // spokes from the centre
    {600, 600, 600, 240}, {600, 600, 920, 440}, {600, 600, 900, 800},
    {600, 600, 580, 980}, {600, 600, 280, 820}, {600, 600, 280, 420},
    // the ring
    {600, 240, 920, 440}, {920, 440, 900, 800}, {900, 800, 580, 980},
    {580, 980, 280, 820}, {280, 820, 280, 420}, {280, 420, 600, 240},
};

static node_t const nodes[] = {
    {600, 600, 100, THEME_AMBER}, {600, 600, 45, THEME_TEAL}, // centre: an amber node with a teal heart
    {600, 240, 70, THEME_AMBER},  {900, 800, 70, THEME_AMBER}, {280, 820, 60, THEME_AMBER},
    {920, 440, 65, THEME_TEAL},   {580, 980, 65, THEME_TEAL},  {280, 420, 70, THEME_TEAL},
};

#define VIEW_X10 1200

static int32_t scale(int32_t v, int32_t size) {
    return (v * size + VIEW_X10 / 2) / VIEW_X10;
}

static void draw(lv_event_t *e) {
    lv_obj_t   *obj   = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t   box;
    lv_obj_get_coords(obj, &box);
    int32_t size = lv_area_get_width(&box);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color       = lv_color_hex(THEME_DIM);
    line.opa         = LV_OPA_80;
    line.width       = LV_MAX(1, scale(25, size));
    line.round_start = 1;
    line.round_end   = 1;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        line.p1.x = box.x1 + scale(lines[i].x1, size);
        line.p1.y = box.y1 + scale(lines[i].y1, size);
        line.p2.x = box.x1 + scale(lines[i].x2, size);
        line.p2.y = box.y1 + scale(lines[i].y2, size);
        lv_draw_line(layer, &line);
    }

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        int32_t r = scale(nodes[i].r, size);
        if (r < 1 || (i == 1 && r >= scale(nodes[0].r, size))) {
            continue; // at 16 px the centre's teal heart would cover its amber node: amber stays
        }
        int32_t   cx   = box.x1 + scale(nodes[i].x, size);
        int32_t   cy   = box.y1 + scale(nodes[i].y, size);
        lv_area_t area = {cx - r, cy - r, cx + r - 1, cy + r - 1};
        dot.bg_color   = lv_color_hex(nodes[i].color);
        lv_draw_rect(layer, &dot, &area);
    }
}

lv_obj_t *logo_create(lv_obj_t *parent, int size_px) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size_px, size_px);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}
