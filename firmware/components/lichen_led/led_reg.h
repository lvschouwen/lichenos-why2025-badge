#pragma once
// Registry getter, led.pad: the GPIO output register level plus polarity, as text (#319). Declared here so reg_p4_misc.c and the definition share one prototype.
#include "reg.h"

reg_status_t led_reg_pad(reg_entry_t const *e, reg_value_t *out);
