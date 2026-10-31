#pragma once

#include <stdbool.h>

// Crash records (#74). Call once at boot after the storage drive is mounted: if the last reset was a crash it is
// appended to /storage/crash/log.txt (the last 20 are kept) and counted in NVS. Also adds the "fw crash" commands.
void crash_check(void);

// This boot follows a crash (for the boot screen)
bool crash_recovered(void);
