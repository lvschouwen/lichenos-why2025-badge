#pragma once
// lichen_os's own entry points for main: not part of the gate, apps never see them.

// Hands the settings to the lower layers that keep a value of their own but may not open NVS (#224): the backlight
// levels and the R16 result (board), the sensors' start-at-boot flag. Call once after NVS is up, before
// backlight_restore().
void settings_bind_services(void);
