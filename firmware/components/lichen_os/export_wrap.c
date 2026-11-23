// Wrappers of IDF and LVGL calls an SD app resolves under the original names (#533, mapped in tools/export_allow.txt), where
// the original would let an app do more than it should. Not host-tested: they only forward after a check on hardware types.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define APP_DELAY_MAX_MS 1000 // an app sleeps in slices: a longer delay would outlast the unload's wait for its tasks

// vTaskDelay, at most a second per call: the unload waits for the app's tasks only a few seconds (a sleeping task that wakes
// late is a faulted unload), and DOOM's DG_SleepMs asks for milliseconds anyway
void lichen_exp_vtaskdelay(TickType_t ticks) {
    TickType_t cap = pdMS_TO_TICKS(APP_DELAY_MAX_MS);
    vTaskDelay(ticks > cap ? cap : ticks);
}

// lv_canvas_set_palette writes into the canvas's draw buffer's palette without looking: a canvas that is not one, one with
// no buffer yet or a buffer that is not indexed would write past or into pixels. LVGL task only, like the original.
void lichen_exp_lv_canvas_set_palette(lv_obj_t *obj, uint8_t index, lv_color32_t color) {
    if (!obj || !lv_obj_check_type(obj, &lv_canvas_class)) {
        return;
    }
    lv_draw_buf_t *buf = lv_canvas_get_draw_buf(obj);
    if (!buf) {
        return;
    }
    unsigned entries = 0;
    switch (buf->header.cf) {
    case LV_COLOR_FORMAT_I1:
        entries = 2;
        break;
    case LV_COLOR_FORMAT_I2:
        entries = 4;
        break;
    case LV_COLOR_FORMAT_I4:
        entries = 16;
        break;
    case LV_COLOR_FORMAT_I8:
        entries = 256;
        break;
    default:
        return; // not an indexed format: there is no palette
    }
    if (index < entries) {
        lv_canvas_set_palette(obj, index, color);
    }
}
