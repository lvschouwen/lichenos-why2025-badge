#pragma once

#include <stdbool.h>

// Which way a power off goes (#281, #292). ESP-free, so host_tests/power_route_test.c sees the same rule main.c runs.
//   SHELL     the Power app's path: the goodbye screen, the apps stop, the saves, then the power hook
//   HEADLESS  main's path without a shell (no display): the bounded saves, the SD card, then off
//   BUSY      a power off is already under way: nothing to do
// A thermal shutdown also takes over a Power-app restart that is already under way (the Power app turns it into a
// power off); a person's `fw poweroff` is told the badge is already going down.

typedef enum { POWER_ROUTE_SHELL, POWER_ROUTE_HEADLESS, POWER_ROUTE_BUSY } power_route_t;

// shell_up: the shell started (it has a display); shell_going_down: its Power app is restarting or powering off;
// headless_going_down: main's headless power off runs; thermal: the thermal watchdog asks
power_route_t power_route(bool shell_up, bool shell_going_down, bool headless_going_down, bool thermal);
