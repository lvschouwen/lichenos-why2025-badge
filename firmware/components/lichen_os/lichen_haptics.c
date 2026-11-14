// lichen_haptics.h for apps and the shell (#221): the gate's calls, defined here over the haptics driver (L2) so no
// caller links to a driver; "fw vib" keeps the driver's haptics_* names
#include "lichen_haptics.h"
#include "lichen_sys_app.h"

#include "haptics.h"

// The gate's enums are the driver's, value for value: a cast is the whole mapping
_Static_assert((int)LICHEN_HAPTIC_TICK == HP_TICK && (int)LICHEN_HAPTIC_NOTIFY == HP_NOTIFY &&
                   (int)LICHEN_HAPTIC_ALERT == HP_ALERT,
               "lichen_haptic_t follows hp_pattern_t");
_Static_assert((int)LICHEN_HAPTIC_LOW == HP_LOW && (int)LICHEN_HAPTIC_MEDIUM == HP_MEDIUM &&
                   (int)LICHEN_HAPTIC_HIGH == HP_HIGH,
               "lichen_haptic_strength_t follows hp_strength_t");

void lichen_haptics_play(lichen_haptic_t pattern) {
    // #229: the shell's own rows play as the OS or as a SYSTEM app, so only an app without HAPTICS is refused
    if ((int)pattern >= 0 && (int)pattern < HP_COUNT && lichen_sys_caps_self_ok(CAP_HAPTICS, "lichen_haptics_play")) {
        haptics_play((hp_pattern_t)pattern);
    }
}

bool lichen_haptics_enabled(void) {
    return haptics_enabled();
}

void lichen_haptics_set_enabled(bool on) {
    haptics_set_enabled(on);
}

lichen_haptic_strength_t lichen_haptics_strength(void) {
    return (lichen_haptic_strength_t)haptics_strength();
}

void lichen_haptics_set_strength(lichen_haptic_strength_t s) {
    haptics_set_strength((hp_strength_t)s); // the driver ignores one out of range
}
