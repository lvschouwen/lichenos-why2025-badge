#include "reg_p4.h"

// The P4's registry keys of domain 0x14: haptics.*, led.*, tz.zone, hold.* (#319, row file split #315). All read-only,
// read from the hardware or libc by the owning components (no ESP call in this file, so the host test links it; the
// stubs are host_tests/reg_stubs_misc.c). The issue calls tz.zone "tz": a registry name needs two dotted segments.
reg_status_t haptics_reg_duty(reg_entry_t const *e, reg_value_t *out); // lichen_haptics: ledc_get_duty as percent
reg_status_t led_reg_pad(reg_entry_t const *e, reg_value_t *out);      // lichen_led: GPIO output register + polarity
reg_status_t clock_reg_tz(reg_entry_t const *e, reg_value_t *out);     // lichen_clock: getenv("TZ")
reg_status_t c6link_reg_hold(reg_entry_t const *e, reg_value_t *out);  // c6link: the LP pad hold latch of GPIO12/13

static reg_entry_t const rows[] = {
    {.id = REG_ID_HAPTICS_DUTY, .type = REG_T_U8, .flags = REG_F_RO, .name = "haptics.duty", .unit = "%", .min = 0,
     .max = 100, .get_applied = haptics_reg_duty},
    {.id = REG_ID_LED_PAD, .type = REG_T_STR, .flags = REG_F_RO, .name = "led.pad", .get_applied = led_reg_pad},
    {.id = REG_ID_TZ, .type = REG_T_STR, .flags = REG_F_RO, .name = "tz.zone", .get_applied = clock_reg_tz},
    {.id = REG_ID_HOLD_GPIO12, .type = REG_T_BOOL, .flags = REG_F_RO, .name = "hold.gpio12",
     .get_applied = c6link_reg_hold},
    {.id = REG_ID_HOLD_GPIO13, .type = REG_T_BOOL, .flags = REG_F_RO, .name = "hold.gpio13",
     .get_applied = c6link_reg_hold},
};

reg_p4_dom_t const reg_p4_misc = {.rows = rows, .count = sizeof(rows) / sizeof(rows[0])};
