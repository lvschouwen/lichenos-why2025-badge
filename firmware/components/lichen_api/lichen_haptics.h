#pragma once
// The vibration motor (#68): a short buzz. The OS owns the motor (GPIO8, PWM) and the person's setting; notifications
// buzz by themselves (lichen_notify.h: not under do-not-disturb, not for silent ones, rate limited), so an app does
// not call this for a notice. An app may use it for feedback of its own; it is silent when the person switched
// vibration off (Settings > Notifications > Vibration) or while an update is received. Non-blocking, safe from any task
// (never from an ISR); a new call replaces a buzz still running.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LICHEN_HAPTIC_TICK,   // 40 ms, a key-press feel
    LICHEN_HAPTIC_NOTIFY, // two short buzzes
    LICHEN_HAPTIC_ALERT,  // two long buzzes
} lichen_haptic_t;

typedef enum { LICHEN_HAPTIC_LOW, LICHEN_HAPTIC_MEDIUM, LICHEN_HAPTIC_HIGH } lichen_haptic_strength_t;

void lichen_haptics_play(lichen_haptic_t pattern);

// The person's setting (default on). Switching it off stops a buzz at once.
bool lichen_haptics_enabled(void);
void lichen_haptics_set_enabled(bool on); // saved
lichen_haptic_strength_t lichen_haptics_strength(void);
void lichen_haptics_set_strength(lichen_haptic_strength_t s); // saved; default high

#ifdef __cplusplus
}
#endif
