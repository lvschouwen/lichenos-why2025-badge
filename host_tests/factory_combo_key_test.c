// Host test (#488): whether the boot factory-reset combo sees a key as injected. Built twice: a release-like build
// (injected stays injected: a DANGER confirm is hands only, #391) and with CONFIG_LICHEN_BENCH_OPEN (the bench's keys
// count).
#include "factory_combo_key.h"

#include <stdio.h>

int main(void) {
    int failures = 0;
#ifdef CONFIG_LICHEN_BENCH_OPEN
    bool const want_injected = false;
#else
    bool const want_injected = true;
#endif
    if (factory_combo_key_injected(true) != want_injected) {
        printf("FAIL: an injected key is seen as %d\n", factory_combo_key_injected(true));
        failures++;
    }
    if (factory_combo_key_injected(false)) { // a hand's key is never injected
        printf("FAIL: a hand's key is seen as injected\n");
        failures++;
    }
    if (failures) {
        return 1;
    }
    printf("factory_combo_key_test ok\n");
    return 0;
}
