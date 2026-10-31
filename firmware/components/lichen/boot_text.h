#pragma once

#include "boot_steps.h"

#include <stddef.h>

// The calm boot screen's words and bar (#66). Pure, host-tested in host_tests/boot_text_test.c.

// The one human line for a step, written into buf (always terminated, cut if cap is small) and returned. pct counts
// only for the C6 image update, pass -1 otherwise.
char const *boot_step_sentence(boot_step_t step, int pct, char *buf, size_t cap);
// Steps that are done, failed or skipped over all steps, 0..100. The clock is left out: it waits on SNTP and may
// stay pending for good.
int boot_overall_pct(boot_view_t const *v);
