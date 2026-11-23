// Host stand-in for export_file.c (#533): log lines are counted by the test, not printed.
#pragma once
extern int host_log_count;
#define ESP_LOGW(tag, ...) ((void)(tag), host_log_count++)
#define ESP_LOGE(tag, ...) ((void)(tag), host_log_count++)
#define ESP_LOGI(tag, ...) ((void)(tag))
