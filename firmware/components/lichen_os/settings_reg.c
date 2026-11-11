#include "settings_reg.h"

#include "settings_fail.h"

reg_status_t settings_reg_write_fail(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = settings_fail_count();
    return REG_OK;
}

// "<key> 0x<code>", "" before the first failure; the uptime goes to the log, a registry string is 15 characters
reg_status_t settings_reg_last_err(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    settings_fail_last_text(out->s, sizeof(out->s));
    return REG_OK;
}
