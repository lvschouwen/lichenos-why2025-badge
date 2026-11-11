#pragma once
// Registry getter, hold.gpio12 / hold.gpio13: the LP pad-hold latch (#319). Declared here so reg_p4_misc.c and the definition share one prototype.
#include "reg.h"

reg_status_t c6link_reg_hold(reg_entry_t const *e, reg_value_t *out);
