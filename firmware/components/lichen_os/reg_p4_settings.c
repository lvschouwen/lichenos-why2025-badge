#include "reg_p4.h"

#include "settings_reg.h"

// The P4's registry keys of domain 0x15: settings.* (write status) (#320, row file split #315). Both read-only, served
// from settings_fail.c's counter (no ESP call in this file, so the host test links it; the stubs are
// host_tests/reg_stubs_settings.c).
static reg_entry_t const rows[] = {
    {.id = REG_ID_SETTINGS_WRITE_FAIL, .type = REG_T_U32, .flags = REG_F_RO, .name = "settings.write_fail",
     .get_applied = settings_reg_write_fail},
    {.id = REG_ID_SETTINGS_LAST_ERR, .type = REG_T_STR, .flags = REG_F_RO, .name = "settings.last_err",
     .get_applied = settings_reg_last_err},
};

reg_p4_dom_t const reg_p4_settings = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
