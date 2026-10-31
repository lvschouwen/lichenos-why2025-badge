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

// A keycap with a K on it
void icon_keytest(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, size_px * 5 / 8, size_px * 5 / 8);
    lv_obj_set_style_radius(cap, size_px / 10, 0);
    lv_obj_set_style_border_width(cap, size_px >= 64 ? 4 : 2, 0);
    lv_obj_set_style_border_color(cap, lv_color_hex(THEME_TEAL), 0);
    lv_obj_center(cap);
    lv_obj_t *k = lv_label_create(cap);
    lv_obj_set_style_text_color(k, lv_color_hex(THEME_TEXT), 0);
    lv_obj_set_style_text_font(k, size_px >= 64 ? lichen_font(LICHEN_FONT_LARGE) : lichen_font(LICHEN_FONT_SMALL), 0);
    lv_label_set_text(k, "K");
    lv_obj_center(k);
}
