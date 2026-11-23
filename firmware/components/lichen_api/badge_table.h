#pragma once
// The tile badge counts behind lichen_badge.h (#102). Since #520 each count lives in its app's block (app_state.h), so no
// table sizes or bounds them; what is left to decide purely is whether a new count is a change. Pure, host-tested in
// host_tests/badge_table_test.c; lichen_badge.c adds the lock and finds the app.

#include <stdbool.h>
#include <stdint.h>

// Sets *count to new_count. True when it changed (and *generation moved, which is how home knows to redraw); the same
// count again changes nothing.
bool badge_count_set(uint32_t *count, uint32_t *generation, uint32_t new_count);
