#pragma once

#include <stdbool.h>
#include <stdint.h>

// Factory reset's system surface (#227, #66): Settings > Factory reset and the boot combo go through here, never to
// lichen_boot. System surface: tools/layers.conf forbids lichen_sys_* in L6. A reset is asked for by saving it as
// pending and restarting (the caller restarts); the next boot does it before anything else reads NVS or the drive.
// The levels repeat factory_plan.h's (lichen_os checks that they stay equal).

typedef enum {
    LICHEN_FACTORY_NONE     = 0,
    LICHEN_FACTORY_SETTINGS = 1, // NVS only
    LICHEN_FACTORY_ALL      = 2, // NVS and the storage drive, erased from flash
} lichen_factory_level_t;

// Saves level as pending; the caller restarts the badge. False when NVS refused (nothing will be erased). Any task
// but not an interrupt (NVS write).
bool lichen_sys_factory_request(lichen_factory_level_t level);
// "settings" or "all" (the console's words), for the log. Any task.
char const *lichen_sys_factory_level_name(lichen_factory_level_t level);

// The combo held at boot: two keys at the ends of the top row held together, offering "settings". Pure state machine;
// the caller owns the state and the clock (ms since the keyboard came up).
typedef enum { LICHEN_FC_IDLE, LICHEN_FC_HOLDING, LICHEN_FC_ARMED, LICHEN_FC_OVER } lichen_fc_state_t;
typedef enum { LICHEN_FC_EV_NONE, LICHEN_FC_EV_ARM, LICHEN_FC_EV_CONFIRM, LICHEN_FC_EV_CANCEL } lichen_fc_event_t;

typedef struct {
    lichen_fc_state_t state;
    bool              a_down, b_down;
    uint32_t          since;
} lichen_fc_combo_t;

// Any task.
void lichen_sys_factory_combo_init(lichen_fc_combo_t *c);
// A key event at now_ms. Any task.
lichen_fc_event_t lichen_sys_factory_combo_key(lichen_fc_combo_t *c, uint8_t code, bool pressed, uint32_t now_ms);
// The clock alone: the hold completing, the question timing out, the window closing. Any task.
lichen_fc_event_t lichen_sys_factory_combo_tick(lichen_fc_combo_t *c, uint32_t now_ms);
// Keys go to the combo's screen only, not the UI. Any task.
bool lichen_sys_factory_combo_swallows(lichen_fc_combo_t const *c);
