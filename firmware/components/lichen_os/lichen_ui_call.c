// lichen_ui.h's calls that belong to the UI service rather than the gate (#84 T8a, moved out of lichen_api by #220):
// handing work to the LVGL task, and building rows into a screen's group. Apps never take the LVGL port lock
// themselves; #221 puts these two behind the gate's ops table.
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
