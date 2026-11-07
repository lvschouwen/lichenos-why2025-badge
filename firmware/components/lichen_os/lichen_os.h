#pragma once
// lichen_os's own entry points for main: not part of the gate, apps never see them.

// Hands the settings to the lower layers that keep a value of their own but may not open NVS (#224): the backlight
// levels and the R16 result (board), the sensors' start-at-boot flag, and (#235) the notification,
// status LED and vibration settings and the restart history. Only stores pointers. Call once, right after
// nvs_boot() and before anything can restart or read a setting (main.c start_nvs).
void settings_bind_services(void);
