#pragma once

#include <stdbool.h>

// The status LED's setting, the shell's side (#244, #216 decision 1): Settings > Notifications > Status LED goes
// through here, never to lichen_led. System surface: tools/layers.conf forbids lichen_sys_* in L6.

// The status LED follows the notices (Settings > Notifications > Status LED). Any task.
bool lichen_sys_led_enabled(void);
// Switches it on or off; saved in NVS. Any task but not an interrupt.
void lichen_sys_led_set_enabled(bool on);
