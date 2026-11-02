// lichen_ui.h's calls that are the API's own rather than the shell's (#84 T8a): handing work to the LVGL task, and
// building rows into a screen's group. Apps never take the LVGL port lock themselves.
#include "lichen_ui.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"

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
