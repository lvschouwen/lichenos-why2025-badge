// The debug lock-owner check (#225, CONFIG_LICHEN_UI_LOCK_ASSERT): LVGL touched from a task that is neither the LVGL
// task nor inside lichen_sys_ui_lock is the #118 class of bug (a list or heap walk racing the LVGL task). LVGL is
// "touched" where nearly every widget change passes: its allocator (create, delete, styles, timers, async calls) and
// lv_inv_area (every redraw request). The linker wraps them (--wrap in CMakeLists.txt, only with the option on); calls
// inside one LVGL source file are not wrapped, which only hides LVGL's own internal calls, never a caller's.
// Cost per wrapped call: one atomic load and a compare while unarmed, plus xTaskGetCurrentTaskHandle and one more load
// once armed (some 20-30 cycles), a few hundred calls per frame at most: well under 0.1 ms of a 33 ms frame.
#include "sdkconfig.h"

#if CONFIG_LICHEN_UI_LOCK_ASSERT
#include "ui_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <assert.h>

static char const TAG[] = "ui";

void       *__real_lv_malloc(size_t size);
void       *__real_lv_malloc_zeroed(size_t size);
void       *__real_lv_realloc(void *p, size_t size);
void        __real_lv_free(void *p);
lv_result_t __real_lv_inv_area(lv_display_t *disp, lv_area_t const *area);

void       *__wrap_lv_malloc(size_t size);
void       *__wrap_lv_malloc_zeroed(size_t size);
void       *__wrap_lv_realloc(void *p, size_t size);
void        __wrap_lv_free(void *p);
lv_result_t __wrap_lv_inv_area(lv_display_t *disp, lv_area_t const *area);

static void check(char const *what) {
    if (xPortInIsrContext() || ui_service_lock_check() != UI_LOCK_VIOLATION) {
        return; // an ISR's task handle is the interrupted task's; LVGL has no business there, but this is not the test
    }
    ESP_LOGE(TAG, "LVGL %s on task %s without the LVGL lock (lichen_sys_ui_lock or lichen_ui_call)", what,
             pcTaskGetName(NULL));
    assert(!"LVGL touched without the LVGL lock (#225)");
}

void *__wrap_lv_malloc(size_t size) {
    check("lv_malloc");
    return __real_lv_malloc(size);
}

void *__wrap_lv_malloc_zeroed(size_t size) {
    check("lv_malloc_zeroed");
    return __real_lv_malloc_zeroed(size);
}

void *__wrap_lv_realloc(void *p, size_t size) {
    check("lv_realloc");
    return __real_lv_realloc(p, size);
}

void __wrap_lv_free(void *p) {
    check("lv_free");
    __real_lv_free(p);
}

lv_result_t __wrap_lv_inv_area(lv_display_t *disp, lv_area_t const *area) {
    check("lv_inv_area");
    return __real_lv_inv_area(disp, area);
}
#endif
