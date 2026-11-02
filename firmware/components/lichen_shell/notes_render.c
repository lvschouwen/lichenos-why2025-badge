// Release notes text as LVGL labels (#13): "# " is the heading, "## " a section, bullets hang under a dot, paragraphs wrap.
// ASCII plus the bullet dot only: the fonts cover that.
#include "fonts.h"
#include "notes_app.h"
#include "release_notes.h"
#include "theme.h"

#define BULLET_W 32

static lv_obj_t *add_label(lv_obj_t *parent, char const *text, uint32_t color, lv_font_t const *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) {
        lv_obj_set_style_text_font(label, font, 0);
    }
    lv_label_set_text(label, text);
    return label;
}

static void add_bullet(lv_obj_t *parent, char const *text) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_scrollable(row, false);
    lv_obj_t *dot = lv_label_create(row);
    lv_obj_set_width(dot, BULLET_W);
    lv_obj_set_style_text_color(dot, lv_color_hex(THEME_TEAL), 0);
    lv_label_set_text(dot, "\xE2\x80\xA2");
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_TEXT), 0);
    lv_label_set_text(label, text);
}

void notes_render(lv_obj_t *parent, char const *text, char const *date) {
    char const *cursor = text;
    rn_block_t  b; // 260 bytes, on the LVGL task's stack for the call only
    bool        dated = date == NULL;
    for (;;) {
        rn_next_block(&cursor, &b);
        switch (b.kind) {
            case RN_END:
                return;
            case RN_H1:
                add_label(parent, b.text, THEME_TEXT, lichen_font(LICHEN_FONT_LARGE));
                if (!dated) {
                    add_label(parent, date, THEME_DIM, NULL);
                    dated = true;
                }
                break;
            case RN_H2:
                add_label(parent, b.text, THEME_AMBER, NULL);
                break;
            case RN_BULLET:
                add_bullet(parent, b.text);
                break;
            case RN_PARA:
                add_label(parent, b.text, THEME_TEXT, NULL);
                break;
        }
    }
}
