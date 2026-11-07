// lichen_ui.h's calls that are the API's own rather than the shell's (#84 T8a): handing work to the LVGL task, and
// building rows into a screen's group. Apps never take the LVGL port lock themselves.
#include "lichen_ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fonts.h"
#include "theme.h"
#include "ui_text.h"

#include <stdarg.h>

static char const TAG[] = "ui";

#define UI_CALL_LOCK_MS 2000 // as the dialog: a render takes milliseconds, a lock held this long is a stuck LVGL task

bool lichen_ui_call(void (*fn)(void *), void *arg) {
    if (!fn) {
        return false;
    }
    // lv_async_call creates an LVGL timer: done outside the lock it races the LVGL task's own timer list (#118)
    if (!lvgl_port_lock(UI_CALL_LOCK_MS)) {
        ESP_LOGW(TAG, "ui call %p not queued: the LVGL lock was not free in %d ms", (void *)fn, UI_CALL_LOCK_MS);
        return false;
    }
    lv_result_t res = lv_async_call(fn, arg);
    lvgl_port_unlock();
    if (res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "ui call %p not queued: out of memory", (void *)fn);
        return false;
    }
    return true;
}

void lichen_ui_with_group(lv_group_t *group, void (*build)(void *), void *arg) {
    lv_group_t *previous = lv_group_get_default();
    lv_group_set_default(group);
    build(arg);
    lv_group_set_default(previous); // in one place, so it can never be left set to a screen that is gone
}

lv_obj_t *lichen_icon_box(lv_obj_t *parent, int32_t w, int32_t h, int32_t x, int32_t y, uint32_t color, int32_t radius) {
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, x, y);
    return b;
}

void lichen_label_line_fit(lv_obj_t *label) {
    // DOT only cuts at the label's height: without one line's height LVGL 9 wraps the text instead. The height is the
    // whole box, so padding and border go on top, or they eat the descenders (the "g" of a heading, #133)
    int32_t h = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN));
    h += lv_obj_get_style_pad_top(label, LV_PART_MAIN) + lv_obj_get_style_pad_bottom(label, LV_PART_MAIN);
    h += 2 * lv_obj_get_style_border_width(label, LV_PART_MAIN);
    lv_obj_set_height(label, h);
}

lv_obj_t *lichen_label_line(lv_obj_t *parent, int32_t width) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lichen_label_line_fit(label);
    return label;
}

lv_obj_t *lichen_label_wrap(lv_obj_t *parent, int32_t width) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

#define HEADING_NAME_LINES 2
#define HEADING_GAP        4
#define HINT_MAX           128
#define HINT_GAP           8 // between the end of the content and the hint (#142)
#define HINT_EDGE          (THEME_PAD / 2) // gap between the hint and the screen's bottom edge

// How many lines the wrapped name takes at the width the heading will have, capped (the label then ends in "...").
// Measured with the font's own metrics because a DOT label needs a fixed height to cut at (#135).
static int32_t name_height(lv_obj_t *parent, char const *name, lv_font_t const *font) {
    lv_obj_update_layout(parent);
    int32_t width = lv_obj_get_content_width(parent);
    int32_t line  = lv_font_get_line_height(font);
    if (width <= 0) {
        return line * HEADING_NAME_LINES; // not laid out yet: leave the room
    }
    lv_point_t size;
    lv_text_get_size(&size, name, font, 0, 0, width, LV_TEXT_FLAG_NONE);
    int32_t lines = (size.y + line - 1) / line;
    return line * (lines < 1 ? 1 : lines > HEADING_NAME_LINES ? HEADING_NAME_LINES : lines);
}

lv_obj_t *lichen_heading(lv_obj_t *parent, char const *kind, char const *name) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, HEADING_GAP, 0);
    lv_obj_set_style_pad_bottom(box, 8, 0); // as the one-line menu heading
    lv_obj_set_scrollable(box, false);
    if (kind && kind[0]) {
        lv_obj_t *k = lichen_label_line(box, LV_PCT(100));
        lv_obj_set_style_text_color(k, lv_color_hex(THEME_DIM), 0);
        lv_label_set_text(k, kind);
    }
    lv_font_t const *large = lichen_font(LICHEN_FONT_LARGE);
    char const      *text  = name ? name : "";
    lv_obj_t        *n     = lv_label_create(box);
    lv_obj_set_width(n, LV_PCT(100));
    lv_obj_set_style_text_font(n, large, 0);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_label_set_text(n, text);
    lv_obj_update_layout(box); // the box has its width now; the name's height follows from it
    lv_obj_set_height(n, name_height(box, text, large));
    return box;
}

#define PAGE_HINT_FLAG LV_OBJ_FLAG_USER_2 // marks the screen's hint (USER_1 is text_entry's secret flag)

lv_obj_t *lichen_page_hint_of(lv_obj_t *screen) {
    for (uint32_t i = 0; screen && i < lv_obj_get_child_count(screen); i++) {
        lv_obj_t *child = lv_obj_get_child(screen, (int32_t)i);
        if (lv_obj_has_flag(child, PAGE_HINT_FLAG)) {
            return child;
        }
    }
    return NULL;
}

lv_obj_t *shell_page_hint(lv_obj_t *screen, ...) {
    char    text[HINT_MAX];
    va_list ap;
    va_start(ap, screen);
    ui_hint_vbuild(text, sizeof(text), ap);
    va_end(ap);
    // One hint per screen: screens rebuild their rows (Files, notifications, crash log) and call this again each time;
    // the hint is the screen's child, not the menu's, so the old one would stay and pile up (#135 review)
    for (int32_t i = (int32_t)lv_obj_get_child_count(screen) - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (lv_obj_has_flag(child, PAGE_HINT_FLAG)) {
            lv_obj_delete(child);
        }
    }
    lv_obj_t *label = lichen_label_line(screen, LV_PCT(100));
    lv_obj_add_flag(label, PAGE_HINT_FLAG);
    lv_obj_add_flag(label, LV_OBJ_FLAG_FLOATING); // out of the flex flow and the scroll: it stays where it is put
    lv_obj_set_style_text_font(label, lichen_font(LICHEN_FONT_SMALL), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(THEME_DIM), 0);
    // A page that scrolls as a whole (This is new, a note, a QR code) moves its text under the floating hint: the hint
    // is a solid strip from the content's end to the screen's edge, so it covers what passes below it (#177)
    lv_obj_set_style_bg_color(label, lv_color_hex(THEME_BG), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(label, HINT_GAP, 0);
    lv_obj_set_style_pad_bottom(label, HINT_EDGE, 0);
    lichen_label_line_fit(label); // after the padding: the height is the whole strip
    lv_label_set_text(label, text);
    // The content area ends above the pad we add; the label sits HINT_EDGE off the screen's bottom edge, and the content
    // keeps HINT_GAP clear of it. The line is the font's, not lv_obj_get_height(): the label has no layout yet, so its
    // height reads 0 and the pad came out too small to clear the hint (#142).
    int32_t line = lv_font_get_line_height(lichen_font(LICHEN_FONT_SMALL));
    int32_t keep = lv_obj_get_style_pad_bottom(screen, LV_PART_MAIN);
    int32_t pad  = ui_hint_pad(keep, line, HINT_EDGE, HINT_GAP);
    if (pad != keep) {
        lv_obj_set_style_pad_bottom(screen, pad, 0);
        // Children sized in percent (the menu's 100%) were laid out against the old content height, and a pad change
        // does not re-size them: mark them so the next layout does
        for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
            lv_obj_mark_layout_as_dirty(lv_obj_get_child(screen, i));
        }
    }
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, pad); // the strip ends at the screen's bottom edge
    lv_obj_move_foreground(label); // drawn over whatever scrolls below it
    return label;
}
