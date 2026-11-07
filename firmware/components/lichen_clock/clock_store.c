// The clock's source record across a warm reset (#64): one u8 in Lichen's NVS namespace, read at boot and written
// when a different source sets the clock. RTC no-init memory would be the obvious place, but it does not survive a
// software restart on this P4 (#4). The settings service is bound by lichen_os (#245): lichen_clock includes no gate
// header and opens no NVS.
#include "lichen_clock.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>

#define KEY "clock_src"

static volatile uint8_t latest; // two saves close together: whichever task writes last writes the newest source
static uint8_t (*s_get)(char const *key, uint8_t def);
static bool (*s_set)(char const *key, uint8_t value);

void clock_bind_store(uint8_t (*get)(char const *key, uint8_t def), bool (*set)(char const *key, uint8_t value)) {
    s_get = get;
    s_set = set;
}

static void save_task(void *arg) {
    (void)arg;
    if (s_set) {
        s_set(KEY, latest);
    }
    vTaskDelete(NULL);
}

// SNTP proposes from the lwIP task, whose 3 KB stack is no place for an NVS write: a short-lived task does it. Runs
// a few times per boot at most.
static void save(clock_src_t src) {
    latest = (uint8_t)src;
    if (xTaskCreate(save_task, "clock_rec", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW("clock", "source record not saved: no task");
    }
}

void clock_store_init(void) {
    clock_set_store((clock_src_t)(s_get ? s_get(KEY, CLOCK_SRC_NONE) : CLOCK_SRC_NONE), save);
}
