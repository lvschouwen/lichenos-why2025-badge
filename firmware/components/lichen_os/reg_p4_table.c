#include "reg_p4.h"

#include "backlight_level.h"

static char const *const bl_idle_names[]     = {"bright", "dim", "off"};
static char const *const bl_override_names[] = {"none", "thermal", "flashlight", "pulse"};
// esp_bt_controller_status_t, as the C6's table (c6/main/reg_c6.c)
static char const *const bt_state_names[] = {"idle", "inited", "enabled"};
// wifi_ps_type_t none / min: the two `fw wifi ps` allows (max modem sleep is never set, so it reads "failed")
static char const *const wifi_ps_names[] = {"none", "min"};
// lora_state_t (shared/lora/lora_proto.h)
static char const *const lora_state_names[] = {"unconfigured", "idle", "rx", "error", "tx"};

#define PROXY_RO(id_, type_, name_, unit_, flags_)                                                                     \
    {.id = (id_), .type = (type_), .flags = REG_F_RO | (flags_), .name = (name_), .unit = (unit_),                    \
     .get_applied = reg_p4_proxy_get}
#define RADIO(id_, type_, name_, unit_) PROXY_RO(id_, type_, name_, unit_, REG_F_TX)
#define WIFI_RO(id_, type_, name_, unit_, flags_, min_, max_)                                                          \
    {.id = (id_), .type = (type_), .flags = REG_F_RO | (flags_), .name = (name_), .unit = (unit_), .min = (min_),      \
     .max = (max_), .get_applied = reg_p4_wifi_get}

reg_entry_t const reg_p4_table[] = {
    {.id = REG_ID_BL_DISPLAY, .type = REG_T_U8, .name = "bl.display", .unit = "%", .min = BACKLIGHT_DISPLAY_MIN,
     .max = 100, .get_applied = reg_p4_proxy_get, .set = reg_p4_bl_set, .saved_key = BACKLIGHT_KEY_DISPLAY},
    {.id = REG_ID_BL_KEYBOARD, .type = REG_T_U8, .name = "bl.keyboard", .unit = "%", .min = 0, .max = 100,
     .get_applied = reg_p4_proxy_get, .set = reg_p4_bl_set, .saved_key = BACKLIGHT_KEY_KEYBOARD},
    PROXY_RO(REG_ID_BL_FREQ, REG_T_U32, "bl.freq", "Hz", 0),
    PROXY_RO(REG_ID_BL_BOOT, REG_T_BOOL, "bl.boot", NULL, 0),
    {.id = REG_ID_BL_IDLE, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "bl.idle", .min = 0, .max = 2,
     .enum_names = bl_idle_names, .get_applied = reg_p4_bl_mode_get},
    {.id = REG_ID_BL_OVERRIDE, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "bl.override", .min = 0, .max = 3,
     .enum_names = bl_override_names, .get_applied = reg_p4_bl_mode_get},
    RADIO(REG_ID_LORA_FREQ, REG_T_U32, "lora.freq", "Hz"),
    RADIO(REG_ID_LORA_BW, REG_T_U32, "lora.bw", "Hz"),
    RADIO(REG_ID_LORA_SF, REG_T_U8, "lora.sf", NULL),
    RADIO(REG_ID_LORA_CR, REG_T_U8, "lora.cr", NULL),
    RADIO(REG_ID_LORA_SYNC, REG_T_U8, "lora.sync", NULL),
    RADIO(REG_ID_LORA_PREAMBLE, REG_T_U32, "lora.preamble", "sym"),
    RADIO(REG_ID_LORA_POWER, REG_T_I16, "lora.power", "dBm"),
    {.id = REG_ID_LORA_STATE, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "lora.state", .min = 0, .max = 4,
     .enum_names = lora_state_names, .get_applied = reg_p4_proxy_get},
    {.id = REG_ID_BT_STATE, .type = REG_T_ENUM, .flags = REG_F_RO, .name = "bt.state", .min = 0, .max = 2,
     .enum_names = bt_state_names, .get_applied = reg_p4_proxy_get},
    PROXY_RO(REG_ID_C6_REG_CAPS, REG_T_U32, "c6.reg_caps", NULL, 0),
    // #311: the regulatory country is fixed in firmware (NL), TX power and bandwidth change what goes on air
    WIFI_RO(REG_ID_WIFI_COUNTRY, REG_T_STR, "wifi.country", NULL, REG_F_TX, 2, 2),
    WIFI_RO(REG_ID_WIFI_PROTOCOL, REG_T_U8, "wifi.protocol", NULL, 0, 0, 0),
    WIFI_RO(REG_ID_WIFI_TXPWR, REG_T_I16, "wifi.txpwr", "qdBm", REG_F_TX, -128, 127),
    WIFI_RO(REG_ID_WIFI_BW, REG_T_U8, "wifi.bw", "MHz", REG_F_TX, 0, 0),
    {.id = REG_ID_WIFI_PS, .type = REG_T_ENUM, .name = "wifi.ps", .min = 0, .max = 1, .enum_names = wifi_ps_names,
     .get_applied = reg_p4_wifi_get, .set = reg_p4_wifi_ps_set},
};

size_t const reg_p4_count = sizeof(reg_p4_table) / sizeof(reg_p4_table[0]);

uint16_t const reg_p4_radio_ids[8] = {REG_ID_LORA_FREQ, REG_ID_LORA_BW,       REG_ID_LORA_SF,    REG_ID_LORA_CR,
                                      REG_ID_LORA_SYNC, REG_ID_LORA_PREAMBLE, REG_ID_LORA_POWER, REG_ID_LORA_STATE};

// The core table first, then the row files; reg_p4_psram.c and its siblings define the rest
static reg_p4_dom_t const core = {.rows = reg_p4_table, .count = sizeof(reg_p4_table) / sizeof(reg_p4_table[0])};

reg_p4_dom_t const *const reg_p4_doms[REG_P4_DOMS] = {&core,        &reg_p4_psram, &reg_p4_display,
                                                      &reg_p4_sensors, &reg_p4_misc, &reg_p4_settings};

size_t reg_p4_total(void) {
    size_t n = 0;
    for (size_t d = 0; d < REG_P4_DOMS; d++) {
        n += reg_p4_doms[d]->count;
    }
    return n;
}

reg_entry_t const *reg_p4_nth(size_t i) {
    for (size_t d = 0; d < REG_P4_DOMS; d++) {
        if (i < reg_p4_doms[d]->count) {
            return &reg_p4_doms[d]->rows[i];
        }
        i -= reg_p4_doms[d]->count;
    }
    return NULL;
}

reg_status_t reg_p4_get(reg_entry_t const *e, bool live, reg_value_t *out) {
    reg_get_fn get = e->get_applied;
    for (size_t d = 0; !live && d < REG_P4_DOMS; d++) {
        reg_p4_dom_t const *dom = reg_p4_doms[d];
        if (dom->get_cached && e >= dom->rows && e < dom->rows + dom->count) {
            get = dom->get_cached;
        }
    }
    return reg_get_with(e, get, out);
}

reg_entry_t const *reg_p4_find_id(uint16_t id) {
    for (size_t d = 0; d < REG_P4_DOMS; d++) {
        reg_entry_t const *e = reg_find_id(reg_p4_doms[d]->rows, reg_p4_doms[d]->count, id);
        if (e) {
            return e;
        }
    }
    return NULL;
}

reg_entry_t const *reg_p4_find_name(char const *name) {
    for (size_t d = 0; d < REG_P4_DOMS; d++) {
        reg_entry_t const *e = reg_find_name(reg_p4_doms[d]->rows, reg_p4_doms[d]->count, name);
        if (e) {
            return e;
        }
    }
    return NULL;
}

size_t reg_p4_check(void) {
    size_t total = reg_p4_total();
    if (total > REG_P4_KEYS_MAX) {
        return REG_P4_KEYS_MAX;
    }
    for (size_t d = 0; d < REG_P4_DOMS; d++) {
        size_t bad = reg_table_check(reg_p4_doms[d]->rows, reg_p4_doms[d]->count);
        if (bad != reg_p4_doms[d]->count) {
            return bad; // index inside the domain; only the "all fine" answer must be exact
        }
    }
    for (size_t i = 0; i < total; i++) { // the first match of an id or name is the key itself: unique across domains
        reg_entry_t const *e = reg_p4_nth(i);
        if (reg_p4_find_id(e->id) != e || reg_p4_find_name(e->name) != e) {
            return i;
        }
    }
    return total;
}
