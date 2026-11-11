#pragma once
// Failed NVS writes of the sanctioned early-boot owners (#325: crash, c6link, lichen_boot, lichen_nvs, later
// psram_guard). They sit at L0..L3 beside or below lichen_os, so they cannot call settings_fail_note() themselves:
// they log the failure here once and hand it to a sink that lichen_os binds at boot (settings_fail_note), so it counts
// in the registry's settings.write_fail / settings.last_err like a failure in the settings service. A failure noted
// before the bind (the crash counter runs first) is kept and replayed on bind. ESP-free apart from the log line;
// host-tested in host_tests/nvs_fail_test.c. Safe from any task (one spinlock over the small state).
#include <stdbool.h>

typedef void (*fw_nvs_fail_sink_fn)(char const *key, int err);

// Binds the sink and replays what was noted before; NULL unbinds (failures are then kept again, up to FW_NVS_FAIL_PENDING)
void fw_nvs_fail_bind(fw_nvs_fail_sink_fn sink);

// err is an esp_err_t from nvs_set_*, nvs_commit or nvs_erase_key. True for ESP_OK. Otherwise logs "<tag>: NVS write of
// <key> failed (0x..)" once and reports it; false.
bool fw_nvs_ok(char const *tag, char const *key, int err);
// The same for nvs_erase_key: a key that is not there (ESP_ERR_NVS_NOT_FOUND) is the wanted end state, so it counts as ok
bool fw_nvs_erased(char const *tag, char const *key, int err);

#define FW_NVS_FAIL_PENDING 4 // failures kept for the bind; more are only logged
// Test hook: forget the sink and the kept failures
void fw_nvs_fail_reset(void);
