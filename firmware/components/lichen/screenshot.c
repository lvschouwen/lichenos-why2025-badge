#include "screenshot.h"

#include "shot_blend.h"

#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define LOCK_MS 2000

static lv_draw_buf_t draw;
static void         *shot_mem;

// Lay one child of a display layer over the picture. The layer itself can't be snapshotted: it is transparent, and
// a snapshot clears (and so skips) a transparent root's own children.
static bool blend_child(lv_obj_t *child, uint8_t *dst, uint32_t w, uint32_t h, uint32_t stride) {
    if (lv_obj_is_hidden(child)) {
        return true;
    }
    lv_obj_update_layout(child);
    lv_draw_buf_t *buf = lv_snapshot_create_draw_buf(child, LV_COLOR_FORMAT_ARGB8888);
    if (!buf) {
        return true; // zero-sized
    }
    lv_draw_buf_clear(buf, NULL); // an opaque child leaves its rounded corners untouched
    bool ok = lv_snapshot_take_to_draw_buf(child, LV_COLOR_FORMAT_ARGB8888, buf) == LV_RESULT_OK;
    if (ok) {
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        int32_t ext = ((int32_t)buf->header.w - lv_obj_get_width(child)) / 2; // the snapshot grew by the shadow/outline size
        shot_blend_argb8888(dst, (int)w, (int)h, stride, buf->data, (int)buf->header.w, (int)buf->header.h,
                            buf->header.stride, a.x1 - ext, a.y1 - ext);
    }
    lv_draw_buf_destroy(buf);
    return ok;
}

static bool blend_layer(lv_obj_t *layer, uint8_t *dst, uint32_t w, uint32_t h, uint32_t stride) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(layer); i++) {
        if (!blend_child(lv_obj_get_child(layer, (int32_t)i), dst, w, h, stride)) {
            return false;
        }
    }
    return true;
}

static int render(lv_obj_t *scr) {
    lv_obj_update_layout(scr);
    uint32_t w      = (uint32_t)lv_obj_get_width(scr);
    uint32_t h      = (uint32_t)lv_obj_get_height(scr);
    uint32_t stride = w * 2;
    size_t   size   = (size_t)stride * h;
    if (w == 0 || h == 0) {
        return 404;
    }
    shot_mem = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shot_mem) {
        return 500;
    }
    if (lv_draw_buf_init(&draw, w, h, LV_COLOR_FORMAT_RGB565, stride, shot_mem, (uint32_t)size) != LV_RESULT_OK ||
        lv_snapshot_take_to_draw_buf(scr, LV_COLOR_FORMAT_RGB565, &draw) != LV_RESULT_OK ||
        draw.header.w != w || draw.header.h != h) {
        return 500;
    }
    return blend_layer(lv_layer_top(), shot_mem, w, h, stride) && blend_layer(lv_layer_sys(), shot_mem, w, h, stride)
               ? 0
               : 500;
}

int screenshot_take(uint8_t const **pixels, uint32_t *width, uint32_t *height, uint32_t *stride) {
    screenshot_free();
    if (!lvgl_port_lock(LOCK_MS)) {
        return 503;
    }
    lv_obj_t *scr = lv_screen_active();
    int       rc  = scr ? render(scr) : 404;
    lvgl_port_unlock();
    if (rc) {
        screenshot_free();
        return rc;
    }
    *pixels = shot_mem;
    *width  = draw.header.w;
    *height = draw.header.h;
    *stride = draw.header.stride;
    return 0;
}

void screenshot_free(void) {
    heap_caps_free(shot_mem);
    shot_mem = NULL;
}
