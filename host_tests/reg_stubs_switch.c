// Host-test stubs for the owners' side of domain 0x16 (switch_reg.h, #501): each put records that it was reached and
// updates the state the matching get reads, so a test can tell which setter a set ended in.
#include "reg_stubs_switch.h"
#include "switch_reg.h"

#include <string.h>

sw_stub_t sw_stub;

static char const *const ui_keys[] = {"saver", "saver_style", "lock", "idle_dim", "idle_off", "kbd_pulse", "home_layout"};
static uint8_t const     ui_defs[] = {1, 0, 0, 6, 30, 1, 0};
static uint8_t           ui_vals[7];
static void            (*hook)(char const *);

void sw_stub_reset(void) {
    memset(&sw_stub, 0, sizeof(sw_stub));
    memcpy(ui_vals, ui_defs, sizeof(ui_vals));
}

void switch_reg_bind_ui(void (*changed)(char const *key)) {
    hook = changed;
}

bool sw_radio_get(unsigned r) {
    return r < 3 && sw_stub.radio[r];
}

bool sw_radio_put(unsigned r, bool on) {
    if (r >= 3) {
        return false;
    }
    sw_stub.radio_puts[r]++;
    sw_stub.radio[r] = on;
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
    return true;
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

void sw_ota_put(bool on) {
    sw_stub.ota_puts++;
    sw_stub.ota = on;
}

uint8_t sw_ota_autooff_get(void) {
    return sw_stub.autooff;
}

void sw_ota_autooff_put(uint8_t m) {
    sw_stub.autooff_puts++;
    sw_stub.autooff = m;
}

static int ui_index(char const *key) {
    for (unsigned i = 0; i < sizeof(ui_keys) / sizeof(ui_keys[0]); i++) {
        if (strcmp(ui_keys[i], key) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool sw_cfg_get(char const *key, uint32_t *out) {
    int i = ui_index(key);
    if (i < 0) {
        return false;
    }
    *out = ui_vals[i];
    return true;
}

bool sw_cfg_put(char const *key, uint32_t value) {
    int i = ui_index(key);
    if (i < 0) {
        return false;
    }
    ui_vals[i] = (uint8_t)value;
    sw_stub.cfg_puts++;
    strncpy(sw_stub.cfg_key, key, sizeof(sw_stub.cfg_key) - 1);
    sw_stub.cfg_val = value;
    if (hook) {
        hook(key);
        sw_stub.ui_changed++;
    }
    return true;
}

bool sw_saved(uint16_t id, char const *saved_key, uint32_t *out) {
    (void)id;
    (void)saved_key;
    (void)out;
    return false;
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

void sw_led_put(bool on) {
    sw_stub.led_on_puts++;
    sw_stub.led_on = on;
}
