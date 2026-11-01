#include "led_pattern.h"

led_pattern_t led_pick(led_inputs_t const *in) {
    switch (in->override) {
    case LED_FORCE_OFF:
        return LED_OFF;
    case LED_FORCE_ON:
        return LED_ON;
    case LED_FORCE_BLINK:
        return LED_SLOW;
    case LED_AUTO:
        break;
    }
    if (!in->enabled) {
        return LED_OFF;
    }
    if (in->updating) {
        return LED_FAST;
    }
    if (in->unread && !in->dnd) {
        return LED_PULSE;
    }
    return LED_OFF;
}

bool led_level(led_pattern_t p, uint32_t t_ms) {
    switch (p) {
    case LED_ON:
        return true;
    case LED_SLOW:
        return t_ms % LED_SLOW_PERIOD_MS < LED_SLOW_PERIOD_MS / 2;
    case LED_FAST:
        return t_ms % LED_FAST_PERIOD_MS < LED_FAST_PERIOD_MS / 2;
    case LED_PULSE:
        return t_ms % LED_PULSE_PERIOD_MS < LED_PULSE_ON_MS;
    case LED_OFF:
    default:
        return false;
    }
}
