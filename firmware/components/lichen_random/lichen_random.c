#include "lichen_random.h"

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"

#include <stdbool.h>
#include <stdint.h>

static char const TAG[]    = "random";
static bool       s_started = false;

extern void __real_bootloader_random_enable(void);

// Log-only for now: shows who outside the OS touches the entropy source (#84)
void __wrap_bootloader_random_enable(void);
void __wrap_bootloader_random_enable(void) {
    ESP_LOGW(TAG, "bootloader_random_enable called from outside the OS, caller %p", __builtin_return_address(0));
    __real_bootloader_random_enable();
}

void lichen_random_start(void) {
    if (s_started) {
        return;
    }
    s_started = true;
    __real_bootloader_random_enable();
}

void lichen_random_fill(void *buf, size_t len) {
    esp_fill_random(buf, len);
}

uint32_t lichen_random_u32(void) {
    return esp_random();
}
