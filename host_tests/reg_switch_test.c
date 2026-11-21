// Host test: the registry's domain 0x16 (#501): the switches, the configured-only UI policy rows and the new RO keys.
// Every row has a getter, the cfg flags are where the owner decided, a registry set (`fw set`, the console's path, and
// lichen_reg_set_u32, the aliases') ends in the owner's own setter, and out-of-range values reach no setter.
#include "reg.h"
#include "reg_cmd.h"
#include "reg_p4.h"
#include "idle_policy.h"
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

// What an alias does (lichen_reg_set_u32 is this on the P4 table): the registry's own pure core
static bool alias_set(char const *name, uint32_t value) {
    return reg_cmd_set_u32(find, name, value);
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
    static char const *const cfg[] = {"ui.saver", "ui.saver_style", "ui.lock", "ui.idle_dim", "ui.idle_off", "ui.kbd_pulse", "ui.layout", "app.bg_off", "radio.bt"};
    static char const *const live[] = {"radio.wifi", "radio.lora", "radio.airplane", "haptics.on", "haptics.strength",
                                       "led.mode", "led.on", "ota.server", "ota.autooff"};
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
            // a cfg row is a saved setting the table knows, with its default; app.bg_off counts a string list, radio.bt has no driver yet
            CHECK(k != NULL || e->id == REG_ID_APP_BG_OFF || e->id == REG_ID_RADIO_BT);
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
    CHECK(fw_set("led.on off") == RC_OK && !sw_stub.led_on && sw_stub.led_on_puts == 1);
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
}

// The shell's hook as shell_ui_policy_changed decides it: whether this set lifts a showing lock (the real policy
// function, with the real saved lock setting behind it)
static char last_hook_key[16];
static int  lifts;
static void hook(char const *key) {
    strncpy(last_hook_key, key, sizeof(last_hook_key) - 1);
    uint32_t lock = 0;
    sw_cfg_get("lock", &lock);
    lifts += idle_key_lifts_lock(key, lock != 0);
}

static void test_ui_policy(void) {
    sw_stub_reset();
    switch_reg_bind_ui(hook);
    reg_value_t v;
    CHECK(reg_get(find("ui.saver"), &v) == REG_OK && v.u == 1); // the default the shell uses
    CHECK(reg_get(find("ui.idle_dim"), &v) == REG_OK && v.u == 6);
    CHECK(reg_get(find("ui.idle_off"), &v) == REG_OK && v.u == 30);
    CHECK(fw_set("ui.lock on") == RC_OK && strcmp(sw_stub.cfg_key, "lock") == 0 && sw_stub.cfg_val == 1);
    CHECK(reg_get(find("ui.lock"), &v) == REG_OK && v.u == 1);
    CHECK(fw_set("ui.saver_style digital") == RC_OK && strcmp(sw_stub.cfg_key, "saver_style") == 0 && sw_stub.cfg_val == 1);
    CHECK(fw_set("ui.idle_off 0") == RC_OK && strcmp(sw_stub.cfg_key, "idle_off") == 0 && sw_stub.cfg_val == 0);
    CHECK(fw_set("ui.kbd_pulse all") == RC_OK && sw_stub.cfg_val == 2);
    // The shell is told which setting changed: only the lock's own off lifts a showing lock (shell_ui_policy_changed)
    CHECK(fw_set("ui.idle_dim 12") == RC_OK && strcmp(last_hook_key, "idle_dim") == 0);
    CHECK(lifts == 0); // none of the sets above lifted a lock (ui.lock on, saver_style, idle_off, kbd_pulse, idle_dim)
    CHECK(fw_set("ui.lock off") == RC_OK && strcmp(last_hook_key, "lock") == 0 && lifts == 1); // only this one does
    CHECK(fw_set("ui.saver off") == RC_OK && lifts == 1);
}

// The saved column (sw_saved, the real code over the fake store): a stored value shows, a default does not
static void test_saved_column(void) {
    sw_stub_reset();
    uint32_t v = 99;
    CHECK(!sw_saved(REG_ID_UI_LOCK, "lock", &v));
    CHECK(fw_set("ui.lock on") == RC_OK && sw_saved(REG_ID_UI_LOCK, "lock", &v) && v == 1);
    CHECK(fw_set("ui.idle_dim 0") == RC_OK && sw_saved(REG_ID_UI_IDLE_DIM, "idle_dim", &v) && v == 0); // 0 is stored
    sw_stub.radio_saved[1] = true;
    CHECK(sw_saved(REG_ID_RADIO_LORA, NULL, &v) && v == 1); // the person's switch
    CHECK(sw_saved(REG_ID_RADIO_AIRPLANE, NULL, &v) && v == 0);
}

// An owner that cannot save says so: the registry answers failed, never a value that will not survive a restart
static void test_save_failures(void) {
    sw_stub_reset();
    sw_stub.save_fails = true;
    CHECK(fw_set("ota.server on") == RC_FAILED && fw_set("ota.autooff 5") == RC_FAILED && fw_set("led.on on") == RC_FAILED);
    CHECK(fw_set("haptics.on off") == RC_FAILED);
    // The reply says the switch took effect, only the save failed
    reg_entry_t const *e;
    reg_value_t        applied;
    bool               have;
    char               reply[160];
    reg_cmd_result_t   r = reg_cmd_set(find, "haptics.on on", NULL, &e, &applied, &have);
    reg_cmd_set_reply(r, e, &applied, have, "haptics.on on", reply, sizeof(reply));
    CHECK(r == RC_FAILED && strstr(reply, "failed: haptics.on set, applied, not saved") != NULL);
    sw_stub.save_fails  = false;
    sw_stub.store_fails = true; // the settings store itself
    CHECK(fw_set("ui.lock on") == RC_FAILED && sw_stub.cfg_puts == 0);
}

static void test_new_ro_keys(void) {
    reg_value_t v;
    sw_stub.bg_off = 2;
    CHECK(reg_get(find("app.bg_off"), &v) == REG_OK && v.u == 2);
    CHECK(fw_set("app.bg_off 0") == RC_RO && sw_stub.bg_off == 2); // `fw app background` owns the list
}

int main(void) {
    test_table();
    test_cfg_flags();
    test_ranges_follow_settings_table();
    test_set_reaches_owner();
    test_refused();
    test_ui_policy();
    test_saved_column();
    test_save_failures();
    test_new_ro_keys();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("reg_switch_test: ok\n");
    return 0;
}
