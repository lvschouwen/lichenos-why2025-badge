#pragma once
// Status LED patterns (#74). Pure: what the LED shows follows from a handful of inputs, and every pattern is a
// function of time, so the host test can check both. Calm by design: unread notices give a short pulse every few
// seconds rather than a steady blink; an update in progress blinks fast.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { LED_OFF, LED_ON, LED_SLOW, LED_FAST, LED_PULSE } led_pattern_t;

// A console override ("fw led on|off|blink"); LED_AUTO follows the inputs
typedef enum { LED_AUTO, LED_FORCE_OFF, LED_FORCE_ON, LED_FORCE_BLINK } led_override_t;

typedef struct {
    bool           enabled;  // Settings › Notifications › Status LED
    bool           dnd;      // do-not-disturb: notices stay dark
    bool           unread;   // something is unread
    bool           updating; // a firmware or file update is being received
    led_override_t override;
} led_inputs_t;

#define LED_PULSE_PERIOD_MS 4000
#define LED_PULSE_ON_MS     150
#define LED_SLOW_PERIOD_MS  1000
#define LED_FAST_PERIOD_MS  250

// Override wins. Otherwise setting off = dark; an update blinks fast even under do-not-disturb (the user started it);
// unread pulses unless do-not-disturb
led_pattern_t led_pick(led_inputs_t const *in);
// Whether the LED is lit at t_ms (uptime) for the pattern
bool          led_level(led_pattern_t p, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
