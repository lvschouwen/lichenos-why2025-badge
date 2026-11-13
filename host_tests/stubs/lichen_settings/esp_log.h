// Host stand-in for ESP-IDF's esp_log.h, for lichen_settings_test.c: the arguments are used (so the code's own are not
// unused), the lines go nowhere
#pragma once

static inline void esp_log_stub(char const *tag, char const *fmt, ...) {
    (void)tag;
    (void)fmt;
}

#define ESP_LOGE(tag, ...) esp_log_stub(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) esp_log_stub(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) esp_log_stub(tag, __VA_ARGS__)

static inline char const *esp_err_to_name(int err) {
    (void)err;
    return "error";
}
