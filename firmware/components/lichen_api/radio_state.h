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

typedef enum { RADIO_WIFI, RADIO_LORA, RADIO_BT, RADIO_COUNT } radio_id_t;

#define RADIO_MASK_ALL ((uint8_t)((1u << RADIO_COUNT) - 1u))
// A fresh badge: Bluetooth stays off until the user switches it on (#197)
#define RADIO_MASK_DEFAULT ((uint8_t)(RADIO_MASK_ALL & ~(1u << RADIO_BT)))
// Stored-state version: 0 = written before #197 (BT was on by default), 1 = current
#define RADIO_STATE_VERSION 1

typedef struct {
    uint8_t on;       // bit per radio_id_t: switched on now
    uint8_t saved;    // the bits before airplane mode went on (meaningful while airplane)
    bool    airplane;
} radio_state_t;

// Wi-Fi and LoRa on, Bluetooth off, airplane off: a fresh badge
void radio_state_init(radio_state_t *s);
// What was stored (three NVS values); anything out of range falls back to the defaults
void radio_state_load(radio_state_t *s, uint8_t on, uint8_t saved, uint8_t airplane);
// One-time upgrade of a loaded state from the stored version; returns the version to store.
// Version 0 drops the BT bit from on and saved (once: the stored version is then 1, so a later BT-on survives reboots).
uint8_t radio_state_migrate(radio_state_t *s, uint8_t version);
bool radio_state_get(radio_state_t const *s, radio_id_t id);
// Returns the bits that changed (0 = nothing to do); an unknown id changes nothing
uint8_t radio_state_set(radio_state_t *s, radio_id_t id, bool on);
uint8_t radio_state_set_airplane(radio_state_t *s, bool airplane);

#ifdef __cplusplus
}
#endif
