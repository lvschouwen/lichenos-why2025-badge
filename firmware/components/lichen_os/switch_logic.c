// The pure side of domain 0x16 (#501): the saved settings behind the ui.* rows and the `fw get` saved column. No ESP
// call here (the settings store and the table of settings are pure headers), so the host test links this file with a
// fake store and runs the real code. The owners' adapters are switch_reg.c.
#include "reg_ids.h"
#include "settings_backup.h"
#include "settings_store.h"
#include "switch_reg.h"

#include <stdatomic.h>
#include <string.h>

static _Atomic(void (*)(char const *)) ui_changed; // the shell's hook, bound once by main

void switch_reg_bind_ui(void (*changed)(char const *key)) {
    atomic_store(&ui_changed, changed);
}

static sb_key_t const *cfg_key(char const *key) {
    return sb_find("badge_fw", key);
}

// The saved value, or the default the settings table gives when none is stored
bool sw_cfg_get(char const *key, uint32_t *out) {
    sb_key_t const *k = cfg_key(key);
    if (!k || k->type != SB_U8) {
        return false;
    }
    *out = settings_get_u8(key, (uint8_t)k->def);
    return true;
}

// Range from the same table, then the save, then the shell is told which key changed
bool sw_cfg_put(char const *key, uint32_t value) {
    sb_key_t const *k = cfg_key(key);
    if (!k || k->type != SB_U8 || value < k->min || value > k->max || !settings_set_u8(key, (uint8_t)value)) {
        return false;
    }
    void (*changed)(char const *) = atomic_load(&ui_changed);
    if (changed) {
        changed(key); // the shell re-reads its policy (idle times, saver style, lock)
    }
    return true;
}

// NVS has no "is it stored": a stored value answers the same to two different defaults
static bool cfg_stored(char const *key, uint32_t *out) {
    uint8_t a = settings_get_u8(key, 0), b = settings_get_u8(key, 1);
    if (a != b) {
        return false;
    }
    *out = a;
    return true;
}

bool sw_saved(uint16_t id, char const *saved_key, uint32_t *out) {
    if (id >= REG_ID_RADIO_WIFI && id <= REG_ID_RADIO_BT) {
        *out = sw_radio_saved(id - REG_ID_RADIO_WIFI);
        return true;
    }
    if (id == REG_ID_RADIO_AIRPLANE) {
        *out = sw_airplane_get();
        return true;
    }
    return saved_key && cfg_stored(saved_key, out);
}
