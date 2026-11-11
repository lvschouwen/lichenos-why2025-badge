// Host-test stubs for the getters/setters that reg_p4_misc.c points at (#312). Only the misc agent edits this file.
#include "reg_p4.h"

#include <string.h>

// The values the host test pins through `fw get`-style reads: motor at rest, LED pad low on an active-high LED, the
// home zone, GPIO12 held and GPIO13 not
reg_status_t haptics_reg_duty(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 0;
    return REG_OK;
}

reg_status_t led_reg_pad(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    memcpy(out->s, "low/off", sizeof("low/off"));
    return REG_OK;
}

reg_status_t clock_reg_tz(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    memcpy(out->s, "CET-1CEST", sizeof("CET-1CEST"));
    return REG_OK;
}

reg_status_t c6link_reg_hold(reg_entry_t const *e, reg_value_t *out) {
    out->u = e->id == REG_ID_HOLD_GPIO12;
    return REG_OK;
}
