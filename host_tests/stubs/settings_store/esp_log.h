// Host stand-in for ESP-IDF's esp_log.h, for settings_store_test.c: the log lines go nowhere
#pragma once

#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))

static inline char const *esp_err_to_name(int err) {
    (void)err;
    return "error";
}
