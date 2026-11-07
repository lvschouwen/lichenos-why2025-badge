#pragma once

#include <stdbool.h>

// The crash record's system surface (#227): the boot screen and the notices ask whether this boot follows a crash.
// System surface: tools/layers.conf forbids lichen_sys_* in L6. The Crash log app reads the log through
// lichen_crashlog.h.

// This boot follows a crash (decided early at boot). Any task.
bool lichen_sys_crash_recovered(void);
