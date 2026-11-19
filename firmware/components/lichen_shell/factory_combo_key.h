#pragma once
// Whether a key counts as injected for the boot factory-reset combo (#391, #488): a hand-only DANGER confirm, except in
// a developer image with the bench-open switch, where the bench's injected keys (USB console, within the 10 s window)
// arm and confirm it. Pure, so the host test pins both builds.
#include <stdbool.h>

#if __has_include("sdkconfig.h") // host tests have none and pass CONFIG_LICHEN_BENCH_OPEN by hand
#include "sdkconfig.h"
#endif
// ESP-IDF does not force-include sdkconfig.h: a file that tests the switch must include it itself, or the dev image
// silently builds the release behaviour (#488)
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET)
#error "sdkconfig.h not seen: CONFIG_LICHEN_BENCH_OPEN would read as off"
#endif

static inline bool factory_combo_key_injected(bool injected) {
#ifdef CONFIG_LICHEN_BENCH_OPEN
    (void)injected;
    return false;
#else
    return injected;
#endif
}
