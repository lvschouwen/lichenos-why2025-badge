#include "dev_tap.h"

#if __has_include("sdkconfig.h") // host tests have none and pass CONFIG_LICHEN_BENCH_OPEN by hand
#include "sdkconfig.h"
#endif
// ESP-IDF does not force-include sdkconfig.h: a file that tests the switch must include it itself, or the dev image
// silently builds the release behaviour (#488)
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET)
#error "sdkconfig.h not seen: CONFIG_LICHEN_BENCH_OPEN would read as off"
#endif

void dev_tap_reset(dev_tap_t *t) {
    t->n    = 0;
    t->last = 0;
}

bool dev_tap(dev_tap_t *t, uint32_t now) {
    if (t->n > 0 && (uint32_t)(now - t->last) > DEV_TAP_GAP_MS) {
        t->n = 0;
    }
    t->last = now;
    if (++t->n >= DEV_TAP_COUNT) {
        t->n = 0;
        return true;
    }
    return false;
}

dev_enter_action_t dev_enter_action(dev_tap_t *t, bool dev_on, bool injected, uint32_t now) {
#ifndef CONFIG_LICHEN_BENCH_OPEN // the developer image's bench-open switch (#488): injected taps count there
    if (injected) {
        return DEV_ENTER_REFUSED;
    }
#else
    (void)injected;
#endif
    if (dev_on) {
        t->n = 0;
        return DEV_ENTER_ASK_OFF;
    }
    return dev_tap(t, now) ? DEV_ENTER_SWITCH_ON : DEV_ENTER_NONE;
}
