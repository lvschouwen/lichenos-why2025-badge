#pragma once
// Registry getter, haptics.duty: the PWM duty the LEDC channel drives, in percent (#319). Declared here so reg_p4_misc.c and the definition share one prototype.
#include "reg.h"

reg_status_t haptics_reg_duty(reg_entry_t const *e, reg_value_t *out);
