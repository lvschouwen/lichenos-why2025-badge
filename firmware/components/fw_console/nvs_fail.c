#include "nvs_fail.h"

#include <stdint.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#define NOT_FOUND ESP_ERR_NVS_NOT_FOUND
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#else
#define NOT_FOUND 0x1102 // ESP_ERR_NVS_BASE + 2
#define LOCK()
#define UNLOCK()
#endif

typedef struct {
    char key[16]; // an NVS key is at most 15 characters
    int  err;
} kept_t;

static fw_nvs_fail_sink_fn sink;
static kept_t              kept[FW_NVS_FAIL_PENDING];
static unsigned            kept_n;

void fw_nvs_fail_reset(void) {
    LOCK();
    sink   = NULL;
    kept_n = 0;
    UNLOCK();
}

void fw_nvs_fail_bind(fw_nvs_fail_sink_fn fn) {
    kept_t   copy[FW_NVS_FAIL_PENDING];
    unsigned n = 0;
    LOCK();
    sink = fn;
    if (fn) {
        n = kept_n;
        memcpy(copy, kept, sizeof(copy));
        kept_n = 0;
    }
    UNLOCK();
    for (unsigned i = 0; i < n; i++) { // outside the lock: the sink takes its own
        fn(copy[i].key, copy[i].err);
    }
}

static void report(char const *key, int err) {
    fw_nvs_fail_sink_fn fn;
    LOCK();
    fn = sink;
    if (!fn && kept_n < FW_NVS_FAIL_PENDING) {
        strncpy(kept[kept_n].key, key, sizeof(kept[0].key) - 1);
        kept[kept_n].key[sizeof(kept[0].key) - 1] = '\0';
        kept[kept_n].err                          = err;
        kept_n++;
    }
    UNLOCK();
    if (fn) {
        fn(key, err);
    }
}

bool fw_nvs_ok(char const *tag, char const *key, int err) {
    if (err == 0) {
        return true;
    }
    key = key ? key : "?";
#ifdef ESP_PLATFORM
    ESP_LOGW(tag ? tag : "nvs", "NVS write of %s failed (0x%x)", key, (unsigned)err);
#else
    (void)tag;
#endif
    report(key, err);
    return false;
}

bool fw_nvs_erased(char const *tag, char const *key, int err) {
    return fw_nvs_ok(tag, key, err == NOT_FOUND ? 0 : err);
}
