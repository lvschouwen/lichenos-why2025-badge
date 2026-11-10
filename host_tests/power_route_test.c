// Host test: which way a power off goes (#281, #292): the shell's Power app when it runs, main's headless path when
// there is no display, nothing twice, and a thermal shutdown wins over a Power-app restart under way
#include "power_route.h"

#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    // #292: no display, so no shell: `fw poweroff` takes the headless path, not the Power app (which restarted)
    CHECK(power_route(false, false, false, false) == POWER_ROUTE_HEADLESS);
    CHECK(power_route(false, false, false, true) == POWER_ROUTE_HEADLESS);
    // with the shell: the Power app's goodbye path
    CHECK(power_route(true, false, false, false) == POWER_ROUTE_SHELL);
    CHECK(power_route(true, false, false, true) == POWER_ROUTE_SHELL);
    // already going down: a person's request does nothing more ...
    CHECK(power_route(true, true, false, false) == POWER_ROUTE_BUSY);
    CHECK(power_route(false, false, true, false) == POWER_ROUTE_BUSY);
    // ... but the thermal shutdown still reaches the Power app, which turns a restart into a power off
    CHECK(power_route(true, true, false, true) == POWER_ROUTE_SHELL);
    // the headless power off is already the way down, thermal or not
    CHECK(power_route(false, false, true, true) == POWER_ROUTE_BUSY);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("power_route_test ok\n");
    return 0;
}
