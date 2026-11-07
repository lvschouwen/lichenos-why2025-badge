// The debug lock-owner check (#225, CONFIG_LICHEN_UI_LOCK_CHECK_MODE): LVGL touched from a task that is neither the LVGL
// task nor inside lichen_sys_ui_lock is the #118 class of bug (a list or heap walk racing the LVGL task). LVGL is
// "touched" where nearly every widget change passes: its allocator (create, delete, styles, timers, async calls) and
// lv_inv_area (every redraw request). The linker wraps them (--wrap in CMakeLists.txt, only with the check built in),
// LVGL's own internal allocations included (they call lv_malloc and friends across source files). Not seen: a call that
// stays inside one LVGL source file (lv_inv_area within lv_refr.c), and property changes that neither allocate nor
// invalidate.
// COUNT mode counts and logs the first hit per call site (ui_lock_hits.c), `fw ui lockcheck` shows them; ASSERT aborts.
// Cost: one load while unarmed; once armed, xTaskGetCurrentTaskHandle and two more loads, some 40-60 cycles per wrapped
// call. A full-screen frame makes about 3-5 k wrapped calls, so 0.3-0.7 ms of a 33 ms frame; an idle frame almost none.
#include "lichen_sys_ui.h"
#include "sdkconfig.h"

#include "fw_console.h"

#include <string.h>

#if CONFIG_LICHEN_UI_LOCK_CHECK
#include "ui_lock_hits.h"
#include "ui_service.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <assert.h>
#include <inttypes.h>

static char const TAG[] = "ui";

static ui_lock_hits_t s_hits;

void       *__real_lv_malloc(size_t size);
void       *__real_lv_malloc_zeroed(size_t size);
void       *__real_lv_calloc(size_t num, size_t size);
void       *__real_lv_zalloc(size_t size);
void       *__real_lv_realloc(void *p, size_t size);
void       *__real_lv_reallocf(void *p, size_t size);
void        __real_lv_free(void *p);
lv_result_t __real_lv_inv_area(lv_display_t *disp, lv_area_t const *area);

void       *__wrap_lv_malloc(size_t size);
void       *__wrap_lv_malloc_zeroed(size_t size);
void       *__wrap_lv_calloc(size_t num, size_t size);
void       *__wrap_lv_zalloc(size_t size);
void       *__wrap_lv_realloc(void *p, size_t size);
void       *__wrap_lv_reallocf(void *p, size_t size);
void        __wrap_lv_free(void *p);
lv_result_t __wrap_lv_inv_area(lv_display_t *disp, lv_area_t const *area);

// site: the wrapped call's return address, i.e. the code that called into LVGL
static void violation(char const *what, uintptr_t site) {
    char const *task = pcTaskGetName(NULL);
    bool        first = ui_lock_hits_record(&s_hits, site, task, what);
#if CONFIG_LICHEN_UI_LOCK_CHECK_ASSERT
    (void)first;
    ESP_LOGE(TAG, "LVGL %s from 0x%08" PRIxPTR " on task %s without the LVGL lock", what, site, task);
    assert(!"LVGL touched without the LVGL lock (#225)");
#else
    if (first) {
        ESP_LOGE(TAG, "LVGL %s from 0x%08" PRIxPTR " on task %s without the LVGL lock (fw ui lockcheck)", what, site,
                 task);
    }
#endif
}

// Inline in each wrapper so __builtin_return_address(0) is the wrapper's caller
#define CHECK(what)                                                                                                   \
    do {                                                                                                              \
        if (!xPortInIsrContext() && ui_service_lock_check() == UI_LOCK_VIOLATION) {                                   \
            violation(what, (uintptr_t)__builtin_return_address(0));                                                  \
        }                                                                                                             \
    } while (0)

void *__wrap_lv_malloc(size_t size) {
    CHECK("lv_malloc");
    return __real_lv_malloc(size);
}

void *__wrap_lv_malloc_zeroed(size_t size) {
    CHECK("lv_malloc_zeroed");
    return __real_lv_malloc_zeroed(size);
}

void *__wrap_lv_calloc(size_t num, size_t size) {
    CHECK("lv_calloc");
    return __real_lv_calloc(num, size);
}

void *__wrap_lv_zalloc(size_t size) {
    CHECK("lv_zalloc");
    return __real_lv_zalloc(size);
}

void *__wrap_lv_realloc(void *p, size_t size) {
    CHECK("lv_realloc");
    return __real_lv_realloc(p, size);
}

void *__wrap_lv_reallocf(void *p, size_t size) {
    CHECK("lv_reallocf");
    return __real_lv_reallocf(p, size);
}

void __wrap_lv_free(void *p) {
    CHECK("lv_free");
    __real_lv_free(p);
}

lv_result_t __wrap_lv_inv_area(lv_display_t *disp, lv_area_t const *area) {
    CHECK("lv_inv_area");
    return __real_lv_inv_area(disp, area);
}

static void report(void) {
#if CONFIG_LICHEN_UI_LOCK_CHECK_ASSERT
    char const *mode = "assert";
#else
    char const *mode = "count";
#endif
    console_reply("ui lockcheck: %s mode, %s, %" PRIu32 " hit(s) without the LVGL lock", mode,
                  ui_service_armed() ? "armed" : "not armed yet", atomic_load(&s_hits.total));
    for (size_t i = 0; i < UI_LOCK_HIT_SITES; i++) {
        ui_lock_hit_site_t *s = &s_hits.sites[i];
        if (atomic_load(&s->site) && atomic_load(&s->ready)) {
            console_reply("  %" PRIu32 "x %s from 0x%08" PRIxPTR " on %s", atomic_load(&s->count), s->what,
                          atomic_load(&s->site), s->task);
        }
    }
}

void lichen_sys_ui_lockcheck_cmd(char const *args) {
    while (*args == ' ') {
        args++;
    }
    if (strcmp(args, "reset") == 0) {
        ui_lock_hits_reset(&s_hits);
        console_reply("ui lockcheck: counts reset");
        return;
    }
    if (*args) {
        console_reply("ui lockcheck [reset]");
        return;
    }
    report();
}
#else
void lichen_sys_ui_lockcheck_cmd(char const *args) {
    (void)args;
    console_reply("ui lockcheck: not built in (CONFIG_LICHEN_UI_LOCK_CHECK_MODE is off)");
}
#endif
