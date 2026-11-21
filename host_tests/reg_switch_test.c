// Host test: the registry's domain 0x16 (#501): the switches, the configured-only UI policy rows and the new RO keys.
// Every row has a getter, the cfg flags are where the owner decided, a registry set (`fw set`, the console's path, and
// lichen_reg_set_u32, the aliases') ends in the owner's own setter, and out-of-range values reach no setter.
#include "reg.h"
#include "reg_cmd.h"
#include "reg_p4.h"
#include "reg_stubs_switch.h"
#include "settings_backup.h"
#include "switch_reg.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// reg_cmd_set looks keys up through reg_p4_find_name; this test links the one domain
reg_entry_t const *find(char const *name) {
    return reg_find_name(reg_p4_switch.rows, reg_p4_switch.count, name);
}

static reg_cmd_result_t fw_set(char const *args) {
    reg_entry_t const *e;
    reg_value_t        applied;
    bool               have;
    return reg_cmd_set(find, args, NULL, &e, &applied, &have);
}

// What an alias does (lichen_reg_set_u32): the same reg_set on the found key
static bool alias_set(char const *name, uint32_t value) {
    reg_entry_t const *e = find(name);
    reg_value_t        v = {.u = value}, applied;
    return e && reg_set(e, &v, &applied) == REG_OK;
}

static void test_table(void) {
    CHECK(reg_table_check(reg_p4_switch.rows, reg_p4_switch.count) == reg_p4_switch.count);
    for (size_t i = 0; i < reg_p4_switch.count; i++) {
        reg_entry_t const *e = &reg_p4_switch.rows[i];
        reg_value_t        v;
        CHECK(REG_ID_DOMAIN(e->id) == 0x16);
        CHECK(e->get_applied != NULL);
        CHECK(reg_get(e, &v) == REG_OK); // every new row has a getter that answers
        CHECK((e->set == NULL) == ((e->flags & REG_F_RO) != 0));
    }
}

static void test_cfg_flags(void) {
    static char const *const cfg[] = {"ui.saver", "ui.saver_style", "ui.lock", "ui.idle_dim", "ui.idle_off", "ui.kbd_pulse", "ui.layout"};
    static char const *const live[] = {"radio.wifi", "radio.lora", "radio.bt", "radio.airplane", "haptics.on", "haptics.strength",
                                       "led.mode", "ota.server", "ota.autooff", "sens.boot", "sens.imu_period",
                                       "sens.env_period", "bl.r16"};
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++) {
        CHECK(find(cfg[i]) && (find(cfg[i])->flags & REG_F_CFG));
    }
    for (size_t i = 0; i < sizeof(live) / sizeof(live[0]); i++) {
        CHECK(find(live[i]) && !(find(live[i])->flags & REG_F_CFG));
    }
}

// The ui.* and haptics rows take their range from the settings table the backup and the shell share
static void test_ranges_follow_settings_table(void) {
    for (size_t i = 0; i < reg_p4_switch.count; i++) {
        reg_entry_t const *e = &reg_p4_switch.rows[i];
        sb_key_t const    *k = e->saved_key ? sb_find("badge_fw", e->saved_key) : NULL;
        if (k) {
            CHECK(k->type == SB_U8 && e->min == k->min && e->max == k->max);
        }
        if (e->flags & REG_F_CFG) {
            CHECK(k != NULL); // a cfg row is a saved setting the table knows, with its default
        }
    }
}

static void test_set_reaches_owner(void) {
    sw_stub_reset();
    CHECK(fw_set("radio.wifi on") == RC_OK && sw_stub.radio[0] && sw_stub.radio_puts[0] == 1);
    CHECK(alias_set("radio.wifi", 0) && !sw_stub.radio[0] && sw_stub.radio_puts[0] == 2); // alias: the same setter
    CHECK(fw_set("radio.lora on") == RC_OK && sw_stub.radio[1] && sw_stub.radio_puts[1] == 1);
    CHECK(fw_set("radio.bt on") == RC_OK && sw_stub.radio_puts[2] == 1);
    CHECK(fw_set("radio.airplane on") == RC_OK && sw_stub.airplane && sw_stub.airplane_puts == 1);
    CHECK(fw_set("haptics.on on") == RC_OK && sw_stub.haptics && sw_stub.haptics_puts == 1);
    CHECK(fw_set("haptics.strength medium") == RC_OK && sw_stub.strength == 1);
    CHECK(fw_set("led.mode blink") == RC_OK && sw_stub.led == 3);
    CHECK(fw_set("ota.server on") == RC_OK && sw_stub.ota && sw_stub.ota_puts == 1);
    CHECK(fw_set("ota.autooff 30") == RC_OK && sw_stub.autooff == 30);
    sw_stub.radio_fails = true; // the owner could not save: the registry says failed, not applied
    CHECK(fw_set("radio.lora off") == RC_FAILED);
}

static void test_refused(void) {
    sw_stub_reset();
    CHECK(fw_set("ota.autooff 241") == RC_RANGE && sw_stub.autooff_puts == 0);
    CHECK(fw_set("haptics.strength 3") != RC_OK && sw_stub.strength_puts == 0);
    CHECK(fw_set("haptics.strength loud") == RC_BAD_VALUE && sw_stub.strength_puts == 0);
    CHECK(fw_set("radio.wifi 2") != RC_OK && sw_stub.radio_puts[0] == 0);
    CHECK(fw_set("led.mode 4") != RC_OK && sw_stub.led_puts == 0);
    CHECK(fw_set("ui.saver 2") != RC_OK && sw_stub.cfg_puts == 0);
    CHECK(fw_set("ui.idle_dim 256") != RC_OK && sw_stub.cfg_puts == 0);
    CHECK(fw_set("ui.layout grid") == RC_RO && sw_stub.cfg_puts == 0); // the home rebuilds from Settings only
    CHECK(fw_set("sens.imu_period 50") == RC_RO && fw_set("sens.boot off") == RC_RO && fw_set("bl.r16 fitted") == RC_RO);
}

static void test_ui_policy(void) {
    sw_stub_reset();
    reg_value_t v;
    CHECK(reg_get(find("ui.saver"), &v) == REG_OK && v.u == 1); // the default the shell uses
    CHECK(reg_get(find("ui.idle_dim"), &v) == REG_OK && v.u == 6);
    CHECK(reg_get(find("ui.idle_off"), &v) == REG_OK && v.u == 30);
    CHECK(fw_set("ui.lock on") == RC_OK && strcmp(sw_stub.cfg_key, "lock") == 0 && sw_stub.cfg_val == 1);
    CHECK(reg_get(find("ui.lock"), &v) == REG_OK && v.u == 1);
    CHECK(fw_set("ui.saver_style digital") == RC_OK && strcmp(sw_stub.cfg_key, "saver_style") == 0 && sw_stub.cfg_val == 1);
    CHECK(fw_set("ui.idle_off 0") == RC_OK && strcmp(sw_stub.cfg_key, "idle_off") == 0 && sw_stub.cfg_val == 0);
    CHECK(fw_set("ui.kbd_pulse all") == RC_OK && sw_stub.cfg_val == 2);
}

static void test_new_ro_keys(void) {
    reg_value_t v;
    CHECK(reg_get(find("sens.imu_period"), &v) == REG_OK && v.u == 100);
    CHECK(reg_get(find("sens.env_period"), &v) == REG_OK && v.u == 1000);
    CHECK(reg_get(find("bl.r16"), &v) == REG_OK && v.u == 2);
    CHECK(reg_get(find("sens.boot"), &v) == REG_OK && v.u == 1);
}

int main(void) {
    test_table();
    test_cfg_flags();
    test_ranges_follow_settings_table();
    test_set_reaches_owner();
    test_refused();
    test_ui_policy();
    test_new_ro_keys();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reg_switch_test: ok\n");
    return 0;
}
