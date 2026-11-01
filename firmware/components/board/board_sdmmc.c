#include "board_sdmmc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;
static portMUX_TYPE      make_lock = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t get(void) {
    // Made on first use from whichever task comes first (main at boot), so no start-up order to keep
    portENTER_CRITICAL(&make_lock);
    if (!lock) {
        lock = xSemaphoreCreateRecursiveMutexStatic(&lock_buf);
    }
    portEXIT_CRITICAL(&make_lock);
    return lock;
}

bool board_sdmmc_lock(uint32_t wait_ms) {
    TickType_t ticks = wait_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms);
    return xSemaphoreTakeRecursive(get(), ticks) == pdTRUE;
}

void board_sdmmc_unlock(void) {
    xSemaphoreGiveRecursive(get());
}
