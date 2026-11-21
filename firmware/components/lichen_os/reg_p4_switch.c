#include "reg_p4.h"

#include "reg_cmd.h"
#include "switch_reg.h"

// The P4's registry keys of domain 0x16 (#501): the OS switches (radio, haptics, status LED, update server), the
// configured-only UI policy settings and the settings no key showed before. Pure rows and dispatch: every call goes
// through switch_reg.h, so the host test links this file with recording stubs. Keep ESP calls out of it.
//
// Settable rows end in the owner's own setter (the one its console command calls). The radio rows go through
// lichen_radio_set (sw_radio_put), never around the owners' hooks (#108). radio.wifi and radio.lora read the driver's
// requested state after the holds (not link state); radio.bt is the OS switch (configured-only); the C6's controller follows it as bt.enable (#503). The ui.* rows are configured-only (REG_F_CFG):
// the saved setting as the shell reads it, so the value is exact and no read-back exists to disagree. The names and
// ranges of the saved u8 settings are the ones in lichen_api/settings_backup.c (a host test compares them).

static char const *const strength_names[] = {"low", "medium", "high"};
static char const *const led_names[]      = {"auto", "off", "on", "blink"}; // led_override_t
static char const *const style_names[]    = {"station", "digital"};
static char const *const pulse_names[]    = {"off", "dms", "all"};
static char const *const layout_names[]   = {"tiles", "grid"};

#define RADIO_ROW(id_, name_, flags_)                                                                                  \
    {.id = (id_), .type = REG_T_BOOL, .flags = (flags_), .name = (name_), .max = 1, .get_applied = sw_get, .set = sw_set}
// UI policy rows: saved_key is the badge_fw key the shell reads. RO = no setter (the shell would have to rebuild)
#define UI_ROW(id_, type_, name_, unit_, key_, max_, names_, set_)                                                     \
    {.id = (id_), .type = (type_), .flags = REG_F_CFG | ((set_) ? 0 : REG_F_RO), .name = (name_), .unit = (unit_),    \
     .min = 0, .max = (max_), .enum_names = (names_), .get_applied = sw_get, .set = (set_) ? sw_set : NULL,          \
     .saved_key = (key_)}

static reg_status_t sw_get(reg_entry_t const *e, reg_value_t *out) {
    uint32_t v = 0;
    switch (e->id) {
        case REG_ID_RADIO_WIFI: v = sw_radio_get(0); break;
        case REG_ID_RADIO_LORA: v = sw_radio_get(1); break;
        case REG_ID_RADIO_BT: v = sw_radio_get(2); break;
        case REG_ID_RADIO_AIRPLANE: v = sw_airplane_get(); break;
        case REG_ID_HAPTICS_ON: v = sw_haptics_get(); break;
        case REG_ID_HAPTICS_STR: v = sw_haptics_strength_get(); break;
        case REG_ID_LED_MODE: v = sw_led_mode_get(); break;
        case REG_ID_OTA_SERVER: v = sw_ota_get(); break;
        case REG_ID_OTA_AUTOOFF: v = sw_ota_autooff_get(); break;
        case REG_ID_APP_BG_OFF: v = sw_bg_off_count(); break;
        case REG_ID_LED_ON: v = sw_led_get(); break;
        default: // the ui.* rows: the saved setting
            if (!e->saved_key || !sw_cfg_get(e->saved_key, &v)) {
                return REG_FAILED;
            }
            break;
    }
    out->u = v;
    return REG_OK;
}

static reg_status_t sw_set(reg_entry_t const *e, reg_value_t const *v) {
    bool ok = true;
    switch (e->id) {
        case REG_ID_RADIO_WIFI: ok = sw_radio_put(0, v->u != 0); break;
        case REG_ID_RADIO_LORA: ok = sw_radio_put(1, v->u != 0); break;
        case REG_ID_RADIO_BT: ok = sw_radio_put(2, v->u != 0); break;
        case REG_ID_RADIO_AIRPLANE: ok = sw_airplane_put(v->u != 0); break;
        case REG_ID_HAPTICS_ON: ok = sw_haptics_put(v->u != 0); break;
        case REG_ID_HAPTICS_STR: ok = sw_haptics_strength_put((uint8_t)v->u); break;
        case REG_ID_LED_MODE: sw_led_mode_put((uint8_t)v->u); break;
        case REG_ID_LED_ON: ok = sw_led_put(v->u != 0); break;
        case REG_ID_OTA_SERVER: ok = sw_ota_put(v->u != 0); break;
        case REG_ID_OTA_AUTOOFF: ok = sw_ota_autooff_put((uint8_t)v->u); break;
        default: ok = e->saved_key && sw_cfg_put(e->saved_key, v->u); break;
    }
    if (!ok) { // the owner's change is live (the ui.* store refuses before it changes anything), only the save failed
        reg_cmd_set_why(REG_ID_DOMAIN(e->id) == 0x16 && e->saved_key && (e->flags & REG_F_CFG) ? "not saved"
                                                                                                : "applied, not saved");
    }
    return ok ? REG_OK : REG_FAILED;
}

static reg_entry_t const rows[] = {
    RADIO_ROW(REG_ID_RADIO_WIFI, "radio.wifi", 0),
    RADIO_ROW(REG_ID_RADIO_LORA, "radio.lora", 0),
    RADIO_ROW(REG_ID_RADIO_BT, "radio.bt", REG_F_CFG), // the switch itself; the C6's side is bt.enable (#503)
    RADIO_ROW(REG_ID_RADIO_AIRPLANE, "radio.airplane", 0),
    {.id = REG_ID_HAPTICS_ON, .type = REG_T_BOOL, .name = "haptics.on", .max = 1, .get_applied = sw_get, .set = sw_set,
     .saved_key = "haptics"},
    {.id = REG_ID_HAPTICS_STR, .type = REG_T_ENUM, .name = "haptics.strength", .max = 2, .enum_names = strength_names,
     .get_applied = sw_get, .set = sw_set, .saved_key = "haptic_str"},
    {.id = REG_ID_LED_MODE, .type = REG_T_ENUM, .name = "led.mode", .max = 3, .enum_names = led_names,
     .get_applied = sw_get, .set = sw_set},
    {.id = REG_ID_LED_ON, .type = REG_T_BOOL, .name = "led.on", .max = 1, .get_applied = sw_get, .set = sw_set,
     .saved_key = "status_led"},
    {.id = REG_ID_OTA_SERVER, .type = REG_T_BOOL, .name = "ota.server", .get_applied = sw_get, .set = sw_set,
     .saved_key = "ota_wifi"},
    {.id = REG_ID_OTA_AUTOOFF, .type = REG_T_U8, .name = "ota.autooff", .unit = "min", .max = 240,
     .get_applied = sw_get, .set = sw_set, .saved_key = "ota_auto_min"},
    UI_ROW(REG_ID_UI_SAVER, REG_T_BOOL, "ui.saver", NULL, "saver", 1, NULL, true),
    UI_ROW(REG_ID_UI_SAVER_STYLE, REG_T_ENUM, "ui.saver_style", NULL, "saver_style", 1, style_names, true),
    UI_ROW(REG_ID_UI_LOCK, REG_T_BOOL, "ui.lock", NULL, "lock", 1, NULL, true),
    UI_ROW(REG_ID_UI_IDLE_DIM, REG_T_U8, "ui.idle_dim", "10s", "idle_dim", 255, NULL, true),
    UI_ROW(REG_ID_UI_IDLE_OFF, REG_T_U8, "ui.idle_off", "10s", "idle_off", 255, NULL, true),
    UI_ROW(REG_ID_UI_KBD_PULSE, REG_T_ENUM, "ui.kbd_pulse", NULL, "kbd_pulse", 2, pulse_names, true),
    UI_ROW(REG_ID_UI_LAYOUT, REG_T_ENUM, "ui.layout", NULL, "home_layout", 1, layout_names, false),
    {.id = REG_ID_APP_BG_OFF, .type = REG_T_U8, .flags = REG_F_RO | REG_F_CFG, .name = "app.bg_off",
     .get_applied = sw_get}, // the list is a string in the settings, only its size is a number
};

reg_p4_dom_t const reg_p4_switch = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
