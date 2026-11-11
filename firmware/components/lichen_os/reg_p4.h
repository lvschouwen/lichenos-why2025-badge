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
// At most this many keys over all domains (21 in the core table with #311, room for ~40 more from wave 2, #312):
// `fw get` collects the matching ones in an array of this size; reg_p4_check() and the host test pin it
#define REG_P4_KEYS_MAX 72
// The core table: the keys of domains 0x01-0x10 (bl.*, lora.*, wifi.*, bt.*, c6.*). The P4-side domains 0x11-0x15 live
// in a row file each (#315), so the wave-2 agents of #312 touch only their own: reg_p4_<domain>.c defines its
// reg_p4_dom_t below (rows + count; NULL and 0 while empty, C has no zero-length arrays). Reach the whole table only
// through the functions below, never through reg_p4_table alone.
extern reg_entry_t const reg_p4_table[];
extern size_t const      reg_p4_count;

typedef struct {
    reg_entry_t const *rows;
    size_t             count;
    // A domain whose rows' getters may wait (ask a driver's task, a bus) gives the getter for a cached read here: the
    // driver's last value, never waiting (#341). NULL = its getters never wait.
    reg_get_fn get_cached;
} reg_p4_dom_t;
extern reg_p4_dom_t const reg_p4_psram;    // 0x11 psram.*    reg_p4_psram.c
extern reg_p4_dom_t const reg_p4_display;  // 0x12 disp.*     reg_p4_display.c
extern reg_p4_dom_t const reg_p4_sensors;  // 0x13 sens.*     reg_p4_sensors.c
extern reg_p4_dom_t const reg_p4_misc;     // 0x14 haptics.*, led.*, tz, hold.*   reg_p4_misc.c
extern reg_p4_dom_t const reg_p4_settings; // 0x15 settings.* reg_p4_settings.c
// The domains in iteration order: [0] the core table, [i] for i = 1..5 holds ids of domain 0x10 + i
#define REG_P4_DOMS 6
extern reg_p4_dom_t const *const reg_p4_doms[REG_P4_DOMS];

// Every key of every domain, in table order: its count, the i-th, and the lookups reg_find_id / reg_find_name do on one
size_t             reg_p4_total(void);
reg_entry_t const *reg_p4_nth(size_t i);
reg_entry_t const *reg_p4_find_id(uint16_t id);
reg_entry_t const *reg_p4_find_name(char const *name);
// reg_table_check over the whole table: the index (reg_p4_nth) of the first bad key, or reg_p4_total() when all are fine
// (ids and names unique across the domains, at most REG_P4_KEYS_MAX keys)
size_t reg_p4_check(void);

// A P4 key's applied value, reg_get's checks included, with the caller's choice (#341): live = the getter may ask its
// driver and wait (the console's `fw get`), false = the driver's last value through the domain's get_cached, never
// waiting (lichen_reg_cached_text, so lichen_reg_get and /info). Not for proxies or wifi.* (lichen_reg.c's views).
reg_status_t reg_p4_get(reg_entry_t const *e, bool live, reg_value_t *out);

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
