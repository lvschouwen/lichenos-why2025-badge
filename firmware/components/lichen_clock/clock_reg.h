#pragma once
// Registry getter, tz.zone: getenv("TZ") without the DST rules (#319). Declared here so reg_p4_misc.c and the definition share one prototype.
#include "reg.h"

reg_status_t clock_reg_tz(reg_entry_t const *e, reg_value_t *out);
