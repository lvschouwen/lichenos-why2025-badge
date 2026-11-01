#pragma once

#include "radio_state.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// The OS radio service (#108): Wi-Fi, LoRa and Bluetooth (#73) switches and airplane mode. The OS owns them; apps only
// read the state and may listen for changes. The state is saved in NVS (badge_fw: radio_on, radio_saved, radio_air)
// and applied at boot by main.c before the radios start. Bluetooth has its slot here but no row and no effect before #73.
// Safe from any task; none of the calls block on a radio (the backends do their work on their own tasks).

bool lichen_radio_get(radio_id_t id);
bool lichen_airplane_get(void);
// Switch one radio. Allowed in airplane mode too (it stays on, the radio comes on). True when the state was saved.
bool lichen_radio_set(radio_id_t id, bool on);
// Airplane on: every radio off, the states kept; off: they come back as before
bool lichen_airplane_set(bool on);
char const *lichen_radio_name(radio_id_t id);

// Told about every radio whose state changed, from the task that changed it (keep it short, don't block). At most
// LICHEN_RADIO_LISTENERS; the OS registers the backends that switch the hardware, the status bar and apps just read.
#define LICHEN_RADIO_LISTENERS 4
typedef void (*lichen_radio_cb_t)(radio_id_t id, bool on);
bool lichen_radio_listen(lichen_radio_cb_t cb);
// Calls every listener with the current state: boot applies the saved state to the backends with it
void lichen_radio_apply_all(void);
// "fw wifi on|off", "fw lora on|off", "fw airplane on|off" and the status line; registers the airplane command
void lichen_radio_console_register(void);

#ifdef __cplusplus
}
#endif
