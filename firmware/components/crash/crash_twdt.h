#pragma once
// "crash test twdt [core]" (#479): the pure part, argument parsing and the one-test-at-a-time claim. crash.c runs the
// task. Compiled only with CONFIG_LICHEN_DEV_COMMANDS (a release image carries none of it).

#include <stdbool.h>

#define CRASH_TWDT_CORES 2

// The text after "test twdt": "" is core 0, " 0" or " 1" the core. False for anything else (a third core, a sign,
// trailing text, an empty argument after the space).
bool crash_twdt_parse(char const *rest, int *core);

// Only one test at a time: true when this call took the slot, false while a test runs. Release when the task ends.
bool crash_twdt_claim(void);
void crash_twdt_release(void);
