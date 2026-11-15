// See walk_pace.h (#471): the walk's clock and its block, on the badge
#include "walk_pace.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdatomic.h>

static _Atomic uint32_t s_worst_us;

void walk_pace_begin(walk_slice_t *s) {
    walk_slice_begin(s, esp_timer_get_time());
}

void walk_pace(walk_slice_t *s) {
    if (!walk_slice_step(s, esp_timer_get_time())) {
        return;
    }
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
        vTaskDelay(1); // a block, not a yield: taskYIELD would only hand the core to tasks of the same priority
    }
    walk_slice_resume(s, esp_timer_get_time());
}

void walk_pace_end(walk_slice_t *s) {
    walk_slice_step(s, esp_timer_get_time()); // the last stretch counts too
    uint32_t seen = atomic_load(&s_worst_us);
    while (s->worst_us > seen && !atomic_compare_exchange_weak(&s_worst_us, &seen, s->worst_us)) {
    }
}

uint32_t walk_pace_worst_ms(void) {
    return atomic_load(&s_worst_us) / 1000;
}
