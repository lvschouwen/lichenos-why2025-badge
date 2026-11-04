#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keyboard light for a new message and the flashlight (#164). Pure, host-tested in host_tests/attention_policy_test.c;
// attention.c applies it. The notification service already decided whether a notice toasts with feedback (not under
// do-not-disturb, not silent, within its rate), so this only decides per kind and per screen state.

typedef enum { ATTN_OFF, ATTN_DMS, ATTN_ALL, ATTN_MODE_COUNT } attn_mode_t;

#define ATTN_MODE_KEY "kbd_pulse" // u8 in badge_fw
#define ATTN_MODE_DEFAULT ATTN_DMS

attn_mode_t attn_mode_from_u8(uint8_t raw);   // an unknown value is the default
attn_mode_t attn_mode_next(attn_mode_t mode); // Off -> Direct messages -> All -> Off
char const *attn_mode_name(attn_mode_t mode);

// A target an app named "dm:<...>" is a direct message (lichen_notify.h); anything else is a channel or the OS
bool attn_is_dm(char const *target);

typedef struct {
    bool dnd;        // do-not-disturb (kept here too: a caller that skipped the service's feedback flag stays quiet)
    bool screen_lit; // fully bright, no saver: a pulse in a dark pocket or over the clock saver is not wanted
    bool hot;        // the thermal watchdog warns: no extra load
    bool flashlight; // the flashlight already owns the keyboard light
} attn_state_t;

bool attn_wants_pulse(attn_mode_t mode, char const *target, uint8_t notice_level, attn_state_t state);

// The pulse: two blinks of 150 ms with 150 ms between. Returns the keyboard level wanted ATTN_PEAK or 0 at elapsed_ms, or
// -1 once the pattern is over (the caller then restores the person's level).
#define ATTN_PEAK     100
#define ATTN_PULSE_MS 450
int attn_keyboard_at(uint32_t elapsed_ms);

// The flashlight: white screen and keyboard light at full; when hot (the thermal watchdog shed load) the screen stays at
// its minimum and the keyboard light off, like the watchdog's own shed
typedef struct {
    uint8_t display;
    uint8_t keyboard;
} attn_levels_t;
attn_levels_t attn_flash_levels(bool hot);
