// The bench test card's drawing (#352; layout and text rules are test_card_layout.c). Plain LVGL objects, no image.
#include "test_card.h"

#include "fonts.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define INFO_MS 500

typedef struct {
    tcard_mode_t mode;
    lv_obj_t    *marker;
    lv_obj_t    *line1, *line2;
    lv_timer_t  *info_timer;
    lv_display_t *disp;
    uint32_t     frame;
    _Atomic bool flushed; // a frame reached the panel since the marker last stepped
} tcard_t;

static test_card_hooks_t hooks;

void test_card_set_hooks(test_card_hooks_t const *h) {
    hooks = *h;
}

static lv_obj_t *box(lv_obj_t *parent, tcard_rect_t r, uint32_t color) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, r.x, r.y);
    lv_obj_set_size(o, r.w, r.h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// A grating draws its lines itself: 1 px and 2 px lines in dozens would be far too many objects
static void on_grating_draw(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    tcard_grating_t g = tcard_grating((int)(intptr_t)lv_event_get_user_data(e));
    lv_area_t        a;
    lv_obj_get_coords(obj, &a);
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_white();
    tcard_rect_t r;
    for (int i = 0; tcard_grating_line(&g, i, &r); i++) {
        lv_area_t line = {a.x1 + r.x - g.area.x, a.y1 + r.y - g.area.y, a.x1 + r.x - g.area.x + r.w - 1,
                          a.y1 + r.y - g.area.y + r.h - 1};
        lv_draw_rect(lv_event_get_layer(e), &dsc, &line);
    }
}

static void add_grating(lv_obj_t *card, int i) {
    lv_obj_t *o = box(card, tcard_grating(i).area, 0x000000);
    lv_obj_add_event_cb(o, on_grating_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
}

static lv_obj_t *add_info_line(lv_obj_t *card, tcard_rect_t strip, int row) {
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_pos(l, strip.x, strip.y + row * (strip.h / 2));
    lv_obj_set_size(l, strip.w, strip.h / 2);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(l, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

static void update_info(lv_timer_t *timer) {
    tcard_t *t = lv_timer_get_user_data(timer);
    char     hz[40] = "", d[40] = "", k[40] = "", ps[40] = "", l1[72], l2[64];
    uint8_t  rd = 0, rk = 0;
    if (hooks.reg_get) {
        hooks.reg_get("disp.refresh", hz, sizeof(hz));
        hooks.reg_get("bl.display", d, sizeof(d));
        hooks.reg_get("bl.keyboard", k, sizeof(k));
        hooks.reg_get("psram.clk", ps, sizeof(ps)); // #354: the speed the panel was shown at
    }
    if (hooks.display_get) {
        hooks.display_get(&rd, &rk); // the saved levels: what `fw bl` and the sliders asked for
    }
    tcard_info_t in = {hooks.fw_label, hz, d, k, hooks.display_get ? rd : -1, rk, ps};
    tcard_info_text(&in, l1, sizeof(l1), l2, sizeof(l2));
    lv_label_set_text(t->line1, l1);
    lv_label_set_text(t->line2, l2);
}

static void on_display(lv_event_t *e) {
    tcard_t *t = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_FLUSH_FINISH) {
        atomic_store(&t->flushed, true);
        return;
    }
    if (t->marker && atomic_exchange(&t->flushed, false)) {
        tcard_rect_t r = tcard_marker_rect(++t->frame);
        lv_obj_set_x(t->marker, r.x); // moving it invalidates two small boxes: the next frame, one step on
    }
}

static void add_test_pattern(lv_obj_t *card, tcard_t *t) {
    for (int i = 0; i < TCARD_GRID_LINES; i++) {
        box(card, tcard_grid_rect(i, true), 0x404040);
        box(card, tcard_grid_rect(i, false), 0x404040);
    }
    tcard_rect_t c = tcard_circle_rect();
    lv_obj_t    *disc = box(card, c, 0xFFFFFF);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    for (int i = 0; i < TCARD_BARS; i++) {
        box(card, tcard_bar_rect(i), tcard_bar_color(i));
    }
    for (int i = 0; i < TCARD_WEDGE; i++) {
        box(card, tcard_wedge_rect(i), tcard_wedge_color(i));
    }
    for (int i = 0; i < TCARD_GRATINGS; i++) {
        add_grating(card, i);
    }
    for (int i = 0; i < TCARD_CORNER_RECTS; i++) {
        box(card, tcard_corner_rect(i), 0xFFFFFF);
    }
    box(card, tcard_info_rect(), 0x000000);
    t->line1 = add_info_line(card, tcard_info_rect(), 0);
    t->line2 = add_info_line(card, tcard_info_rect(), 1);
    box(card, tcard_marker_track(), 0x404040);
    t->marker = box(card, tcard_marker_rect(t->frame), 0xFFFFFF);
    lv_obj_t *frame = box(card, tcard_border_rect(), 0x000000); // last: on top of everything
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 1, 0);
    lv_obj_set_style_border_color(frame, lv_color_white(), 0);
    t->info_timer = lv_timer_create(update_info, INFO_MS, t);
    update_info(t->info_timer);
}

static void clear_pattern(lv_obj_t *card, tcard_t *t) {
    if (t->info_timer) {
        lv_timer_delete(t->info_timer);
        t->info_timer = NULL;
    }
    t->marker = t->line1 = t->line2 = NULL;
    lv_obj_clean(card);
}

void test_card_set_mode(lv_obj_t *card, tcard_mode_t mode) {
    tcard_t *t = lv_obj_get_user_data(card);
    if (!t || mode >= TCARD_OFF) {
        return;
    }
    clear_pattern(card, t);
    t->mode = mode;
    lv_obj_set_style_bg_color(card, lv_color_hex(tcard_solid_color(mode)), 0);
    if (mode == TCARD_TEST) {
        lv_obj_set_style_bg_color(card, lv_color_black(), 0);
        add_test_pattern(card, t);
    }
}

static void on_card_delete(lv_event_t *e) {
    tcard_t *t = lv_event_get_user_data(e);
    if (hooks.opened) {
        hooks.opened(-1);
    }
    if (t->info_timer) {
        lv_timer_delete(t->info_timer);
    }
    for (uint32_t i = lv_display_get_event_count(t->disp); i > 0; i--) { // both registrations, newest first
        lv_event_dsc_t *dsc = lv_display_get_event_dsc(t->disp, i - 1);
        if (lv_event_dsc_get_cb(dsc) == on_display && lv_event_dsc_get_user_data(dsc) == t) {
            lv_display_remove_event(t->disp, i - 1);
        }
    }
    lv_free(t);
}

lv_obj_t *test_card_create(lv_obj_t *parent, tcard_mode_t mode) {
    tcard_t *t = lv_malloc_zeroed(sizeof(*t)); // 60-odd bytes beside the draw objects, in LVGL's own pool
    if (!t) {
        return NULL;
    }
    t->disp = lv_display_get_default();
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 0, 0);
    lv_obj_set_size(card, TCARD_SIZE, TCARD_SIZE);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_user_data(card, t);
    lv_obj_add_event_cb(card, on_card_delete, LV_EVENT_DELETE, t);
    if (hooks.opened) {
        hooks.opened(1);
    }
    lv_display_add_event_cb(t->disp, on_display, LV_EVENT_FLUSH_FINISH, t);
    lv_display_add_event_cb(t->disp, on_display, LV_EVENT_REFR_READY, t);
    test_card_set_mode(card, mode >= TCARD_OFF ? TCARD_TEST : mode);
    return card;
}
