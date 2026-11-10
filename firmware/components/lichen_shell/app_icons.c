#include "app_icons.h"

#include "logo.h"
#include "fonts.h"
#include "theme.h"

void icon_settings(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *gear = lv_label_create(parent);
    lv_obj_set_style_text_color(gear, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(gear, size_px >= 64 ? lichen_font(LICHEN_FONT_LARGE) : lichen_font(LICHEN_FONT_SMALL), 0);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear);
}

void icon_about(lv_obj_t *parent, int size_px) {
    lv_obj_t *logo = logo_create(parent, size_px);
    lv_obj_center(logo);
}

// Bench (#96): a gauge, three quarters of a ring with the amber needle part filled
void icon_bench(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(arc, size_px * 5 / 8, size_px * 5 / 8);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_value(arc, 70);
    lv_obj_set_style_arc_width(arc, size_px >= 64 ? 6 : 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, size_px >= 64 ? 6 : 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(THEME_FAINT), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(THEME_AMBER), LV_PART_INDICATOR);
    lv_obj_center(arc);
}

// Crash log (#72): a red-ringed tile with an amber "!"
void icon_crashlog(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, size_px * 5 / 8, size_px * 5 / 8);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, size_px >= 64 ? 5 : 3, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(THEME_RED), 0);
    lv_obj_center(ring);
    lv_obj_t *mark = lv_label_create(ring);
    lv_obj_set_style_text_color(mark, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(mark, size_px >= 64 ? lichen_font(LICHEN_FONT_LARGE) : lichen_font(LICHEN_FONT_SMALL), 0);
    lv_label_set_text(mark, "!");
    lv_obj_center(mark);
}

// Notifications (#62): an amber bell
void icon_notify(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *bell = lv_label_create(parent);
    lv_obj_set_style_text_color(bell, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(bell, size_px >= 64 ? lichen_font(LICHEN_FONT_LARGE) : lichen_font(LICHEN_FONT_SMALL), 0);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_center(bell);
}

// Power (#65): the power symbol, a ring open at the top with a bar through the gap
void icon_power(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int       w   = size_px >= 64 ? 5 : 3;
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size_px * 5 / 8, size_px * 5 / 8);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, 300, 240); // 0 = 3 o'clock, clockwise: open between 240 and 300 (the top)
    lv_obj_set_style_arc_width(arc, w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(THEME_TEAL), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(arc);
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, w, size_px * 3 / 8);
    lv_obj_set_style_radius(bar, w / 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(THEME_TEAL), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -size_px / 8);
}

// Release notes (#13): a sheet with a heading line and three text lines
void icon_notes(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(sheet);
    lv_obj_set_size(sheet, size_px * 9 / 16, size_px * 11 / 16);
    lv_obj_set_style_radius(sheet, size_px / 12, 0);
    lv_obj_set_style_border_width(sheet, size_px >= 64 ? 4 : 2, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(THEME_TEAL), 0);
    lv_obj_center(sheet);
    int line_h = size_px >= 64 ? 4 : 2;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *line = lv_obj_create(sheet);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, i == 0 ? size_px * 5 / 16 : size_px * 7 / 32, line_h);
        lv_obj_set_style_bg_color(line, lv_color_hex(i == 0 ? THEME_AMBER : THEME_DIM), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_align(line, LV_ALIGN_TOP_LEFT, size_px / 32, size_px / 24 + i * (size_px * 3 / 32));
    }
}

// Files (#71): an amber folder on the tile
void icon_files(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    int w = size_px * 5 / 8, h = size_px * 7 / 20;
    lv_obj_t *tab = lv_obj_create(parent);
    lv_obj_remove_style_all(tab);
    lv_obj_set_size(tab, w * 2 / 5, h / 3);
    lv_obj_set_style_radius(tab, size_px / 32 + 1, 0);
    lv_obj_set_style_bg_color(tab, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
    lv_obj_align(tab, LV_ALIGN_CENTER, -w / 2 + w / 5, -h / 2 - h / 8);
    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, w, h);
    lv_obj_set_style_radius(body, size_px / 16 + 1, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, h / 8);
}
