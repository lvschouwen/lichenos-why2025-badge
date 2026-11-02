#include "lichen_random.h"

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

static char const TAG[]    = "random";
static atomic_flag s_started = ATOMIC_FLAG_INIT;

extern void __real_bootloader_random_enable(void);

// The only other caller in IDF is nvs_sec_provider, which burns an HMAC key into eFuse from esp_fill_random() while it
// holds the source on: refusing it would burn a weak key for good. It is linked only with NVS encryption, which is off;
// turning that on must allow it here first.
#if CONFIG_NVS_ENCRYPTION
#error "NVS encryption: nvs_sec_provider calls bootloader_random_enable(); allow it in __wrap_bootloader_random_enable"
#endif

// Refuses (#84 T9): the OS turns the entropy source on once (lichen_random_start, through __real_) and owns the ADC
// hand-over (#11, #69); apps take random bytes from lichen_random_fill(). The caller is named by its return address.
void __wrap_bootloader_random_enable(void);
void __wrap_bootloader_random_enable(void) {
    ESP_LOGE(TAG, "bootloader_random_enable refused: the OS owns the entropy source (lichen_random), caller %p",
             __builtin_return_address(0));
}

void lichen_random_start(void) {
    if (atomic_flag_test_and_set(&s_started)) {
        return;
    }
    __real_bootloader_random_enable();
}

void lichen_random_fill(void *buf, size_t len) {
    esp_fill_random(buf, len);
}

uint32_t lichen_random_u32(void) {
    return esp_random();
}
