#pragma once

#include "reg.h"

#include <stdbool.h>
#include <stddef.h>

// The P4's settings registry table (#307/#310): its own keys (bl.idle, bl.override, served from the backlight driver)
// and a proxy entry for every C6 key it mirrors (bl.*, lora.*, c6.reg_caps; asked over REG_GET / REG_SET). Pure data,
// so reg_service_test.c runs reg_table_check and the console set rules on the real table; the getters and the setter
// are lichen_reg.c's on the badge and the test's stubs on the host.
//
// Radio keys are read-only and TX (#307): a set answers GUARDED and never reaches a setter. wifi.* (#311) is the P4's
// own domain 0x03 (the Wi-Fi driver is reached over the esp_hosted RPCs, not REG_GET): wifi.country (fixed NL),
// wifi.txpwr and wifi.bw are TX and change only through `fw wifi tx|bw`; wifi.ps is settable. bt.state is a C6 key.
// At most this many keys (21 with #311): `fw get` collects the matching ones in an array of this size; reg_p4_table.c
// asserts it at compile time
#define REG_P4_KEYS_MAX 32
extern reg_entry_t const reg_p4_table[];
extern size_t const      reg_p4_count;

// A C6 key: the cached REG_VAL, after a short live ask unless the caller may not wait (the LVGL task)
reg_status_t reg_p4_proxy_get(reg_entry_t const *e, reg_value_t *out);
// bl.display / bl.keyboard: through the backlight driver as a saved level (as "fw bl" always did); the driver sends it
reg_status_t reg_p4_bl_set(reg_entry_t const *e, reg_value_t const *v);
// wifi.*: the Wi-Fi service's read-back, live on the console, its cache on the LVGL task
reg_status_t reg_p4_wifi_get(reg_entry_t const *e, reg_value_t *out);
// wifi.ps: none / min through the Wi-Fi service (not saved, as `fw wifi ps`)
reg_status_t reg_p4_wifi_ps_set(reg_entry_t const *e, reg_value_t const *v);
// bl.idle / bl.override from the backlight driver (and the thermal watchdog's load shed)
reg_status_t reg_p4_bl_mode_get(reg_entry_t const *e, reg_value_t *out);

static inline bool reg_p4_is_proxy(reg_entry_t const *e) {
    return e->get_applied == reg_p4_proxy_get;
}

// The ids of the lora.* keys in reg_radio_text's order (reg_cmd.h REG_RADIO_KEYS)
extern uint16_t const reg_p4_radio_ids[8];
