// Host-test stubs for the getters that reg_p4_settings.c points at (#320). Only the settings agent edits this file.
#include "reg_p4.h"

#include "settings_reg.h"

#include <string.h>

// Two failed writes so far, the last of them "wifi_ssid_0" with ESP_ERR_NVS_NOT_ENOUGH_SPACE (0x1106)
reg_status_t settings_reg_write_fail(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = 2;
    return REG_OK;
}

reg_status_t settings_reg_last_err(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    memcpy(out->s, "wifi_ssi 0x1106", sizeof("wifi_ssi 0x1106"));
    return REG_OK;
}
