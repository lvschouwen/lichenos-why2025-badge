#include "app_icons.h"

#include "logo.h"
#include "theme.h"

void icon_settings(lv_obj_t *parent, int size_px) {
    lv_obj_set_style_radius(parent, size_px / 4, 0);
    lv_obj_set_style_bg_color(parent, lv_color_hex(THEME_TRACK), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t *gear = lv_label_create(parent);
    lv_obj_set_style_text_color(gear, lv_color_hex(THEME_AMBER), 0);
    lv_obj_set_style_text_font(gear, size_px >= 64 ? &lv_font_montserrat_40 : &lv_font_montserrat_24, 0);
    lv_label_set_text(gear, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear);
}

void icon_about(lv_obj_t *parent, int size_px) {
    lv_obj_t *logo = logo_create(parent, size_px);
    lv_obj_center(logo);
}
