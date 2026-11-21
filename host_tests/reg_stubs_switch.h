#pragma once
// What the recording stubs of switch_reg.h saw (#501): host_tests/reg_stubs_switch.c
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     radio[3], radio_saved[3], store_fails, save_fails, airplane, haptics, ota;
    uint8_t  strength, led, autooff;
    int      radio_puts[3], airplane_puts, haptics_puts, strength_puts, led_puts, ota_puts, autooff_puts, cfg_puts;
    bool     led_on;
    uint8_t  bg_off;
    int      led_on_puts;
    bool     radio_fails;       // sw_radio_put reports "not saved"
    char     cfg_key[16];       // the last sw_cfg_put
    uint32_t cfg_val;
} sw_stub_t;

extern sw_stub_t sw_stub;
void             sw_stub_reset(void);
