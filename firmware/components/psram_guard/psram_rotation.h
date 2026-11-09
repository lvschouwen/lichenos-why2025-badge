#pragma once

#include <stdbool.h>

// Which canary the guard checks on this pass (#258). One canary per pass instead of all of them keeps the CPU cost at a
// sixth, and every canary is still visited within one round: the worst-case detection time is the number of canaries
// held times the pass period. Pure, host-tested in host_tests/psram_rotation_test.c.

// The next used slot after `after` (wrapping; pass -1 to start at slot 0), or -1 when no slot is used
int psram_rot_next(bool const used[], int n, int after);
