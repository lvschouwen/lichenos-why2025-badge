#pragma once
// Whether a key counts as injected for the boot factory-reset combo (#391, #488): a hand-only DANGER confirm, except in
// a developer image with the bench-open switch, where the bench's injected keys (USB console, within the 10 s window)
// arm and confirm it. Pure, so the host test pins both builds.
#include <stdbool.h>

static inline bool factory_combo_key_injected(bool injected) {
#ifdef CONFIG_LICHEN_BENCH_OPEN
    (void)injected;
    return false;
#else
    return injected;
#endif
}
