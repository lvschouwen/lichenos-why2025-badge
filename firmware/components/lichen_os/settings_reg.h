#pragma once
// Registry getters of domain 0x15 (#320): the settings service's write failures. Declared here so reg_p4_settings.c and
// the definitions (settings_reg.c; host_tests/reg_stubs_settings.c for the table test) share one prototype.
#include "reg.h"

reg_status_t settings_reg_write_fail(reg_entry_t const *e, reg_value_t *out);
reg_status_t settings_reg_last_err(reg_entry_t const *e, reg_value_t *out);
