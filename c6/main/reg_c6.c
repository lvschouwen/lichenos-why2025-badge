// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// Every getter reads what the hardware or the driver runs now: the LEDC duty, the SX1262's state through RadioLib and
// its registers, the BT controller. Nothing here is a copy of what the P4 asked for.
#include "reg_c6.h"

#include "backlight.h"
#include "lora_service.h"

#include <stdbool.h>

static int bl_channel(reg_entry_t const *e) {
    return e->id == REG_ID_BL_DISPLAY ? BL_DISPLAY : BL_KEYBOARD;
}

static reg_status_t bl_get(reg_entry_t const *e, reg_value_t *out) {
    uint8_t pct;
    if (!backlight_applied(bl_channel(e), &pct)) {
        return REG_FAILED;
    }
    out->u = pct;
    return REG_OK;
}

// The same apply path as the UART "BL d k" line, one channel at a time; reg_set reads the duty back afterwards
static reg_status_t bl_set(reg_entry_t const *e, reg_value_t const *v) {
    return backlight_apply(bl_channel(e), (int)v->u) ? REG_OK : REG_FAILED;
}

static reg_status_t bl_freq_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = backlight_freq_hz();
    return out->u ? REG_OK : REG_FAILED;
}

static reg_status_t bl_boot_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = backlight_at_boot();
    return REG_OK;
}

// One key of what the SX1262 runs; FAILED while the radio has not been started (RadioLib holds no settings then) or
// holds a code lora_readback cannot decode
static reg_status_t lora_get(reg_entry_t const *e, reg_value_t *out) {
    lora_config_t c;
    if (!lora_service_readback(&c)) {
        return REG_FAILED;
    }
    switch (e->id) {
        case REG_ID_LORA_FREQ: out->u = c.freq_hz; break;
        case REG_ID_LORA_BW: out->u = c.bw_hz; break;
        case REG_ID_LORA_SF: out->u = c.sf; break;
        case REG_ID_LORA_CR: out->u = c.cr; break;
        case REG_ID_LORA_SYNC: out->u = c.sync_word; break;
        case REG_ID_LORA_PREAMBLE: out->u = c.preamble; break;
        case REG_ID_LORA_POWER: out->i = c.tx_power_dbm; break;
        default: return REG_FAILED;
    }
    return REG_OK;
}

static reg_status_t lora_state_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = lora_service_state();
    return REG_OK;
}

static reg_status_t bt_state_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = c6_bt_status();
    return REG_OK;
}

static reg_status_t caps_get(reg_entry_t const *e, reg_value_t *out) {
    (void)e;
    out->u = REG_CAP_BL | REG_CAP_LORA | REG_CAP_BT | REG_CAP_C6;
    return REG_OK;
}

// lora_state_t, in its order
static char const *const lora_state_names[] = {"unconfigured", "idle", "rx", "error", "tx"};
// esp_bt_controller_status_t, in its order
static char const *const bt_state_names[] = {"idle", "inited", "enabled"};

#define RO_TX (REG_F_RO | REG_F_TX)

// Not saved on the C6 (saved_key NULL): the P4 keeps the saved backlight levels and sends them after every start
reg_entry_t const reg_c6_table[] = {
    {REG_ID_BL_DISPLAY, REG_T_U8, 0, "bl.display", "%", 0, 100, NULL, bl_get, bl_set, NULL},
    {REG_ID_BL_KEYBOARD, REG_T_U8, 0, "bl.keyboard", "%", 0, 100, NULL, bl_get, bl_set, NULL},
    {REG_ID_BL_FREQ, REG_T_U32, REG_F_RO, "bl.freq", "Hz", 0, 0, NULL, bl_freq_get, NULL, NULL},
    {REG_ID_BL_BOOT, REG_T_BOOL, REG_F_RO, "bl.boot", NULL, 0, 0, NULL, bl_boot_get, NULL, NULL},
    {REG_ID_LORA_FREQ, REG_T_U32, RO_TX, "lora.freq", "Hz", 0, 0, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_BW, REG_T_U32, RO_TX, "lora.bw", "Hz", 0, 0, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_SF, REG_T_U8, RO_TX, "lora.sf", NULL, 5, 12, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_CR, REG_T_U8, RO_TX, "lora.cr", NULL, 5, 8, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_SYNC, REG_T_U8, RO_TX, "lora.sync", NULL, 0, 0, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_PREAMBLE, REG_T_U32, RO_TX, "lora.preamble", "sym", 0, 0, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_POWER, REG_T_I16, RO_TX, "lora.power", "dBm", -9, 22, NULL, lora_get, NULL, NULL},
    {REG_ID_LORA_STATE, REG_T_ENUM, REG_F_RO, "lora.state", NULL, 0, 4, lora_state_names, lora_state_get, NULL, NULL},
    {REG_ID_BT_STATE, REG_T_ENUM, REG_F_RO, "bt.state", NULL, 0, 2, bt_state_names, bt_state_get, NULL, NULL},
    {REG_ID_C6_REG_CAPS, REG_T_U32, REG_F_RO, "c6.reg_caps", NULL, 0, 0, NULL, caps_get, NULL, NULL},
};

size_t const reg_c6_count = sizeof(reg_c6_table) / sizeof(reg_c6_table[0]);
