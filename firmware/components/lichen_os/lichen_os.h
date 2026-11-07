#pragma once

#include <stdbool.h>
#include <stddef.h>
// lichen_os's own entry points for main: not part of the gate, apps never see them.

// Hands the settings to the lower layers that keep a value of their own but may not open NVS (#224): the backlight
// levels and the R16 result (board), the sensors' start-at-boot flag, and (#235) the notification,
// status LED and vibration settings and the restart history. Only stores pointers. Call once, right after
// nvs_boot() and before anything can restart or read a setting (main.c start_nvs).
void settings_bind_services(void);

// A small binary value in the same namespace (the restart history, #235): OS-internal, deliberately not in the gate
// (settings_store.h) so apps cannot read or write raw blobs. get is true only when exactly size bytes are stored.
bool settings_get_blob(char const *key, void *out, size_t size);
bool settings_set_blob(char const *key, void const *data, size_t size);

// Hands lichen_net its store (#235): the SSIDs and the wifi_drv_wiped flag as plain settings, the Wi-Fi passwords as
// secrets (settings_secret.h). Call once after NVS is up, before wifi_service_init().
void settings_bind_net(void);
