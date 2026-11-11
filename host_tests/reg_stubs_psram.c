// Host-test stubs for the getters/setters that reg_p4_psram.c points at (#312). Only the psram agent edits this file.
#include "psram_reg.h"
#include "reg_p4.h"

#include <string.h>

// psram_guard's psram_reg_get (#316): the registers are the badge's; here every key reads as 0
reg_status_t psram_reg_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 0;
    return REG_OK;
}

// psram_guard's psram_reg_fail_get (#329): no failure recorded; the STR keys read "none"
reg_status_t psram_reg_fail_get(reg_entry_t const *e, reg_value_t *out) {
    if (e->type == REG_T_STR) {
        memcpy(out->s, "none", sizeof("none"));
    } else {
        out->u = 0;
    }
    return REG_OK;
}
