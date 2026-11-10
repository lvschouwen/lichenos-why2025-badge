#include "power_route.h"

power_route_t power_route(bool shell_up, bool shell_going_down, bool headless_going_down, bool thermal) {
    if (headless_going_down) {
        return POWER_ROUTE_BUSY;
    }
    if (!shell_up) {
        return POWER_ROUTE_HEADLESS;
    }
    return shell_going_down && !thermal ? POWER_ROUTE_BUSY : POWER_ROUTE_SHELL;
}
