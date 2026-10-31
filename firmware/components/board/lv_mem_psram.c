// LVGL's heap (LV_STDLIB_CUSTOM, #80): widgets go to PSRAM. Through plain malloc every LVGL object is smaller than
// SPIRAM_MALLOC_ALWAYSINTERNAL and lands in internal RAM, where the MeshCore lists alone could eat ~100 KB of the
// DMA-capable RAM ESP-Hosted needs. Draw buffers come from esp_lvgl_port with their own caps, so they don't pass here.
#include "lvgl.h"

#include "esp_heap_caps.h"

#define LV_HEAP_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL; // not supported
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

// Internal RAM stays the fallback: the UI keeps working if PSRAM ever runs out
void *lv_malloc_core(size_t size) {
    void *p = heap_caps_malloc(size, LV_HEAP_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
}

void *lv_realloc_core(void *p, size_t new_size) {
    void *q = heap_caps_realloc(p, new_size, LV_HEAP_CAPS);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_DEFAULT);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    LV_UNUSED(mon_p); // "fw mem" reports the heaps
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}
