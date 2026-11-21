// Host-test stubs for the owners' side of domain 0x16 (switch_reg.h, #501): each put records that it was reached and
// updates the state the matching get reads, so a test can tell which setter a set ended in.
#include "reg_stubs_switch.h"
#include "settings_store.h"
#include "switch_reg.h"

#include <stdio.h>
#include <string.h>

sw_stub_t sw_stub;

// A fake settings store under the real switch_logic.c (sw_cfg_get/put, sw_saved, the table of settings_backup.c): a few
// keys, "stored" only after a set, as NVS
#define STORE_MAX 8
static struct {
    char    key[16];
    uint8_t val;
    bool    used;
} store[STORE_MAX];

uint8_t settings_get_u8(char const *key, uint8_t def) {
    for (int i = 0; i < STORE_MAX; i++) {
        if (store[i].used && strcmp(store[i].key, key) == 0) {
            return store[i].val;
        }
    }
    return def;
}

bool settings_set_u8(char const *key, uint8_t value) {
    if (sw_stub.store_fails) {
        return false;
    }
    int free_slot = -1;
    for (int i = 0; i < STORE_MAX; i++) {
        if (store[i].used && strcmp(store[i].key, key) == 0) {
            store[i].val = value;
            return true;
        }
        free_slot = !store[i].used && free_slot < 0 ? i : free_slot;
    }
    if (free_slot < 0) {
        return false;
    }
    strncpy(store[free_slot].key, key, sizeof(store[0].key) - 1);
    store[free_slot].val  = value;
    store[free_slot].used = true;
    sw_stub.cfg_puts++;
    snprintf(sw_stub.cfg_key, sizeof(sw_stub.cfg_key), "%s", key);
    sw_stub.cfg_val = value;
    return true;
}

void sw_stub_reset(void) {
    memset(&sw_stub, 0, sizeof(sw_stub));
    memset(store, 0, sizeof(store));
}

bool sw_radio_get(unsigned r) {
    return r < 3 && sw_stub.radio[r];
}

bool sw_radio_put(unsigned r, bool on) {
    if (r >= 3) {
        return false;
    }
    sw_stub.radio_puts[r]++;
    sw_stub.radio[r]       = on;
    sw_stub.radio_saved[r] = on;
    return !sw_stub.radio_fails;
}

bool sw_airplane_get(void) {
    return sw_stub.airplane;
}

bool sw_airplane_put(bool on) {
    sw_stub.airplane_puts++;
    sw_stub.airplane = on;
    return true;
}

bool sw_haptics_get(void) {
    return sw_stub.haptics;
}

bool sw_haptics_put(bool on) {
    sw_stub.haptics_puts++;
    sw_stub.haptics = on;
    return !sw_stub.save_fails;
}

uint8_t sw_haptics_strength_get(void) {
    return sw_stub.strength;
}

bool sw_haptics_strength_put(uint8_t s) {
    sw_stub.strength_puts++;
    sw_stub.strength = s;
    return true;
}

uint8_t sw_led_mode_get(void) {
    return sw_stub.led;
}

void sw_led_mode_put(uint8_t m) {
    sw_stub.led_puts++;
    sw_stub.led = m;
}

bool sw_ota_get(void) {
    return sw_stub.ota;
}

bool sw_ota_put(bool on) {
    sw_stub.ota_puts++;
    sw_stub.ota = on;
    return !sw_stub.save_fails;
}

uint8_t sw_ota_autooff_get(void) {
    return sw_stub.autooff;
}

bool sw_ota_autooff_put(uint8_t m) {
    sw_stub.autooff_puts++;
    sw_stub.autooff = m;
    return !sw_stub.save_fails;
}

bool sw_radio_saved(unsigned r) {
    return r < 3 && sw_stub.radio_saved[r];
}

bool sw_sensors_boot_get(void) {
    return true;
}

void sw_sensors_periods(uint32_t *imu, uint32_t *env) {
    *imu = 100;
    *env = 1000;
}

uint8_t sw_r16_get(void) {
    return 2;
}

uint8_t sw_bg_off_count(void) {
    return sw_stub.bg_off;
}

bool sw_led_get(void) {
    return sw_stub.led_on;
}

bool sw_led_put(bool on) {
    sw_stub.led_on_puts++;
    sw_stub.led_on = on;
    return !sw_stub.save_fails;
}
