#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The OS radio switches and airplane mode as a pure state machine (#108), no ESP headers (host_tests/radio_state_test.c).
// lichen_radio.c keeps one under a lock, persists it and tells the radios what changed.
//
// Airplane on: the states are saved and every radio goes off; a radio may still be switched on one by one (phone style).
// Airplane off: the radios come back as they were before it was switched on, plus any that were switched on meanwhile.
//
// Holds (#282): the system may hold radios off for a reason (the thermal load shed) without touching the person's
// switches. `on` is the person's choice and the only part that is saved; a held radio reads off (radio_state_get) and
// comes back as `on` says when its reason ends. Holds live in RAM only, so a boot (radio_state_load) starts with none.
// Switching a held radio on is the person overriding the hold for that radio; switching it off is saved as usual.

typedef enum { RADIO_WIFI, RADIO_LORA, RADIO_BT, RADIO_COUNT } radio_id_t;
typedef enum { RADIO_HOLD_NONE, RADIO_HOLD_THERMAL, RADIO_HOLD_COUNT } radio_hold_t;

#define RADIO_MASK_ALL ((uint8_t)((1u << RADIO_COUNT) - 1u))
// A fresh badge: Bluetooth stays off until the user switches it on (#197)
#define RADIO_MASK_DEFAULT ((uint8_t)(RADIO_MASK_ALL & ~(1u << RADIO_BT)))
// Stored-state version: 0 = written before #197 (BT was on by default), 1 = current
#define RADIO_STATE_VERSION 1

typedef struct {
    uint8_t on;       // bit per radio_id_t: the person's switches (what is saved)
    uint8_t saved;    // the bits before airplane mode went on (meaningful while airplane)
    bool    airplane;
    uint8_t held[RADIO_HOLD_COUNT]; // per reason, the radios it holds off; RAM only ([RADIO_HOLD_NONE] stays 0)
} radio_state_t;

// Wi-Fi and LoRa on, Bluetooth off, airplane off, no holds: a fresh badge
void radio_state_init(radio_state_t *s);
// What was stored (three NVS values); anything out of range falls back to the defaults. No holds.
void radio_state_load(radio_state_t *s, uint8_t on, uint8_t saved, uint8_t airplane);
// One-time upgrade of a loaded state from the stored version; returns the version to store.
// Version 0 drops the BT bit from on and saved (once: the stored version is then 1, so a later BT-on survives reboots).
uint8_t radio_state_migrate(radio_state_t *s, uint8_t version);
// What the radio does: switched on and not held
bool    radio_state_get(radio_state_t const *s, radio_id_t id);
uint8_t radio_state_effective(radio_state_t const *s);
// The calls below return the bits whose effective state (radio_state_get) changed, 0 = nothing to tell the radios.
// An unknown id changes nothing; switching a radio on ends every hold of it.
uint8_t radio_state_set(radio_state_t *s, radio_id_t id, bool on);
uint8_t radio_state_set_airplane(radio_state_t *s, bool airplane);
// Holds the radios in mask off for reason (added to what it already holds); RADIO_HOLD_NONE or an unknown reason
// changes nothing
uint8_t radio_state_hold(radio_state_t *s, radio_hold_t reason, uint8_t mask);
// Ends every hold of reason: those radios come back as the person's switches say
uint8_t radio_state_release(radio_state_t *s, radio_hold_t reason);
// The first reason holding id off, RADIO_HOLD_NONE when none does
radio_hold_t radio_state_held_by(radio_state_t const *s, radio_id_t id);

#ifdef __cplusplus
}
#endif
