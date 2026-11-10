#pragma once

#include "radio_state.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The radio service's system holds (#282): the OS holds radios off for a reason (the thermal load shed) without
// touching the person's switches. Nothing here is saved: a boot starts with no hold, and when the reason ends the radios
// come back as the person set them (lichen_radio.h's switches, still saved as before). While held, lichen_radio_get
// reads off; the person switching a held radio on overrides the hold for it. System surface: tools/layers.conf forbids
// lichen_sys_* in L6. Any task but an interrupt; the listeners hear the change from the calling task.

// Holds the radios in mask (bits per radio_id_t) off for reason, on top of what it already holds. Any task.
void lichen_sys_radio_hold(radio_hold_t reason, uint8_t mask);
// Ends every hold of reason. Any task.
void lichen_sys_radio_release(radio_hold_t reason);
// The reason id is held off for, RADIO_HOLD_NONE when it is not held. Any task.
radio_hold_t lichen_sys_radio_held_by(radio_id_t id);

#ifdef __cplusplus
}
#endif
