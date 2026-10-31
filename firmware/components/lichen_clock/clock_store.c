// The clock's source record across a warm reset (#64): one u8 in Lichen's NVS namespace, read at boot and written
// when a different source sets the clock. RTC no-init memory would be the obvious place, but it does not survive a
// software restart on this P4 (#4).
#include "lichen_clock.h"
#include "settings_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdint.h>

#define KEY "clock_src"

static volatile uint8_t latest; // two saves close together: whichever task writes last writes the newest source

static void save_task(void *arg) {
    (void)arg;
    settings_set_u8(KEY, latest);
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
    clock_set_store((clock_src_t)settings_get_u8(KEY, CLOCK_SRC_NONE), save);
}
