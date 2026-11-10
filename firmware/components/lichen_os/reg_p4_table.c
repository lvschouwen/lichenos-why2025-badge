#include "reg_p4.h"

#include "backlight_level.h"

static char const *const bl_idle_names[]     = {"bright", "dim", "off"};
static char const *const bl_override_names[] = {"none", "thermal", "flashlight", "pulse"};
// lora_state_t (shared/lora/lora_proto.h)
static char const *const lora_state_names[] = {"unconfigured", "idle", "rx", "error", "tx"};

#define PROXY_RO(id_, type_, name_, unit_, flags_)                                                                     \
    {.id = (id_), .type = (type_), .flags = REG_F_RO | (flags_), .name = (name_), .unit = (unit_),                    \
     .get_applied = reg_p4_proxy_get}
#define RADIO(id_, type_, name_, unit_) PROXY_RO(id_, type_, name_, unit_, REG_F_TX)

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
    PROXY_RO(REG_ID_C6_REG_CAPS, REG_T_U32, "c6.reg_caps", NULL, 0),
};

size_t const reg_p4_count = sizeof(reg_p4_table) / sizeof(reg_p4_table[0]);

uint16_t const reg_p4_radio_ids[8] = {REG_ID_LORA_FREQ, REG_ID_LORA_BW,       REG_ID_LORA_SF,    REG_ID_LORA_CR,
                                      REG_ID_LORA_SYNC, REG_ID_LORA_PREAMBLE, REG_ID_LORA_POWER, REG_ID_LORA_STATE};
