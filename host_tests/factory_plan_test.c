#include "factory_plan.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define S FACTORY_SETTINGS
#define A FACTORY_ALL

static void boot_plan_per_pending_value(void) {
    factory_boot_t p = factory_boot_plan(0);
    CHECK(!p.nvs && !p.storage);
    p = factory_boot_plan(FACTORY_SETTINGS);
    CHECK(p.nvs && !p.storage && p.after_nvs == 0 && p.level == S);
    p = factory_boot_plan(FACTORY_ALL);
    CHECK(p.nvs && p.storage && p.after_nvs == FACTORY_PENDING_STORAGE && p.level == A);
    CHECK(p.nvs_full && p.restore);
    p = factory_boot_plan(FACTORY_SETTINGS);
    CHECK(!p.nvs_full); // settings: key by key
    p = factory_boot_plan(FACTORY_PENDING_STORAGE); // power cut before the drive's erase: NVS is not erased again
    CHECK(!p.nvs && p.storage && p.restore && p.level == A);
    p = factory_boot_plan(FACTORY_PENDING_ERASING); // power cut during it: the PSRAM copy is gone, erase again
    CHECK(!p.nvs && p.storage && !p.restore && p.level == A);
    p = factory_boot_plan(77);
    CHECK(!p.nvs && !p.storage);
}

// The keep list comes from the apps (#230 E1d): nothing is kept until one is declared, and what an app declares is
// kept in its own namespace only
static void app_declared_keys_are_kept(void) {
    factory_keep_app_keys_clear();
    CHECK(factory_nvs_erases("a.meshcore", "mc_advlast", A)); // no literal in the OS any more
    CHECK(factory_keep_app_key("a.pager", "last_tx"));
    CHECK(!factory_nvs_erases("a.pager", "last_tx", S) && !factory_nvs_erases("a.pager", "last_tx", A));
    CHECK(factory_nvs_erases("a.pager", "other", A) && factory_nvs_erases("a.meshcore", "last_tx", A));
    CHECK(!factory_keep_app_key("a.pager", "0123456789abcdef")); // over the 15 an NVS key may have
    CHECK(!factory_keep_app_key(NULL, "k"));
    CHECK(factory_keep_app_key("a.pager", "last_tx")); // declared twice: one entry, so it uses no second slot
    for (int i = 0; i < 7; i++) { // 8 fit in all: pager/last_tx above and 7 more
        char key[8];
        snprintf(key, sizeof(key), "k%d", i);
        CHECK(factory_keep_app_key("a.x", key));
    }
    CHECK(factory_keep_app_key("a.x", "k0"));  // known: still fine when full
    CHECK(!factory_keep_app_key("a.x", "k9")); // a new one: full
    // An app's stored secret (#288, "~" + key) goes at both levels, even when it reached the keep list
    factory_keep_app_keys_clear();
    CHECK(factory_keep_app_key("a.pager", "~pwabc"));
    CHECK(factory_nvs_erases("a.pager", "~pwabc", S) && factory_nvs_erases("a.pager", "~pwabc", A));
    CHECK(factory_nvs_erases("a.meshcore", "~:moved", A));
    factory_keep_app_keys_clear();
    factory_keep_app_key("a.meshcore", "mc_advlast"); // what MeshCore's record declares
}

static void settings_go_at_both_levels(void) {
    char const *settings[] = {"wifi_ssid", "wifi_pass", "wifi_s0", "wifi_p0", "wifi_s4", "wifi_p4", "tz", "setup_done", "home_layout", "idle_dim", "idle_off",
                              "bl_disp", "bl_kbd", "status_led", "haptics", "haptic_str", "notify_dnd", "n.mesh", "radio_on", "radio_saved",
                              "radio_air", "ota_wifi", "ota_auto_min", "saver", "lock", "mc_advint", "mc_advfl", "some_new_key"};
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        CHECK(factory_nvs_erases("badge_fw", settings[i], S));
        CHECK(factory_nvs_erases("badge_fw", settings[i], A));
    }
    CHECK(factory_nvs_erases("a.meshcore", "first_run", S));
    CHECK(factory_nvs_erases("a.meshcore", "mc_advint", S)); // the advert settings go with the app's (#123)
    CHECK(factory_nvs_erases("a.meshcore", "mc_advfl", A));
    CHECK(factory_nvs_erases("a.other", "mc_advlast", A));   // only MeshCore's own last advert is kept
    CHECK(factory_nvs_erases("a.x", "k", A));
}

static void badge_state_stays(void) {
    char const *keep[] = {"boot_fail", "crash_count", "dump_sum", "rst_log", "psram_rty", "c6_force", "c6_ok_key",
                          "c6_verify", "c6_rt_restart", "upd_from", "batt_seen", "clock_src", "r16", FACTORY_PENDING_KEY};
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        CHECK(!factory_nvs_erases("badge_fw", keep[i], S));
        CHECK(!factory_nvs_erases("badge_fw", keep[i], A));
    }
    // MeshCore's last advert since #123: the advert interval holds across a reset (#52)
    CHECK(!factory_nvs_erases("a.meshcore", "mc_advlast", S));
    CHECK(!factory_nvs_erases("a.meshcore", "mc_advlast", A));
}

// A reset orders the Wi-Fi driver store's clearing (#342): the order survives the erase, so wifi_service can act on it
static void wifi_wipe_order_survives(void) {
    CHECK(strcmp(FACTORY_WIFI_WIPE_KEY, "wifi_drv_wipe") == 0); // wifi_list.c reads the same literal
    CHECK(!factory_nvs_erases("badge_fw", FACTORY_WIFI_WIPE_KEY, S));
    CHECK(!factory_nvs_erases("badge_fw", FACTORY_WIFI_WIPE_KEY, A));
}

static void identity_only_at_all(void) {
    CHECK(factory_nvs_erases("badge_fw", "identity", S)); // a stale private-key copy goes at every level
    CHECK(factory_nvs_erases("badge_fw", "identity", A));
}

// The factory BadgeVMS firmware left its own namespaces (Wi-Fi password included); nothing in Lichen reads them, and a
// reset badge must not keep someone's Wi-Fi password
static void badgevms_leftovers_go(void) {
    CHECK(factory_nvs_erases("badgevms_wifi", "password", S));
    CHECK(factory_nvs_erases("badgevms_wifi", "ssid", A));
    CHECK(factory_nvs_erases("badgevms_init", "sponsors", S));
    CHECK(factory_nvs_erases("backlight", "display", A));
    CHECK(!factory_nvs_erases("badgevms", "k", A)); // only the exact prefix
}

static void other_namespaces_stay(void) {
    CHECK(!factory_nvs_erases("phy", "cal_data", A));
    CHECK(!factory_nvs_erases("nvs.net80211", "sta.ssid", A));
    CHECK(!factory_nvs_erases("a.", "k", A)); // not an app id
    CHECK(!factory_nvs_erases("badge_fw", "tz", FACTORY_NONE));
    CHECK(!factory_nvs_erases(NULL, "tz", A));
}

static void storage_keeps_os_files(void) {
    CHECK(factory_storage_keeps("c6"));
    CHECK(factory_storage_keeps("lora.toml"));
    CHECK(!factory_storage_keeps("meshcore"));
    CHECK(!factory_storage_keeps("lichen"));
    CHECK(!factory_storage_keeps("ota_token.txt"));
    CHECK(!factory_storage_keeps("C6x"));
    CHECK(!factory_storage_keeps(NULL));
}

static void md5_files_go_back_last(void) {
    CHECK(factory_restore_late("firmware.bin.md5"));
    CHECK(!factory_restore_late("firmware.bin"));
    CHECK(!factory_restore_late("md5"));
    CHECK(!factory_restore_late(NULL));
}

static void storage_erase_fails_then_retries_then_gives_up(void) {
    factory_storage_next_t n = factory_storage_after(true, 0);
    CHECK(!n.fallback_wipe && n.pending == 0 && n.tries == 0 && !n.notice);
    n = factory_storage_after(true, 2); // worked on a retry: the count goes too
    CHECK(!n.fallback_wipe && n.pending == 0 && n.tries == 0 && !n.notice);
    n = factory_storage_after(false, 0);
    CHECK(n.fallback_wipe && n.pending == FACTORY_PENDING_STORAGE && n.tries == 1 && !n.notice);
    n = factory_storage_after(false, 1);
    CHECK(n.fallback_wipe && n.pending == FACTORY_PENDING_STORAGE && n.tries == 2 && !n.notice);
    n = factory_storage_after(false, FACTORY_STORAGE_TRIES - 1); // the last try: stop, and say so
    CHECK(n.fallback_wipe && n.pending == 0 && n.tries == 0 && n.notice);
    n = factory_storage_after(false, 200); // a stale count never loops
    CHECK(n.pending == 0 && n.notice);
}

static void nvs_flag_write_fails_continues_in_this_boot(void) {
    factory_nvs_next_t n = factory_nvs_after(true, true);
    CHECK(!n.key_fallback && !n.carry_in_ram);
    n = factory_nvs_after(true, false);
    CHECK(!n.key_fallback && n.carry_in_ram);
    n = factory_nvs_after(false, true);
    CHECK(n.key_fallback && !n.carry_in_ram);
    n = factory_nvs_after(false, false);
    CHECK(n.key_fallback && n.carry_in_ram);
    CHECK(!factory_nvs_erases("badge_fw", FACTORY_TRIES_KEY, FACTORY_ALL));
}

static void level_words(void) {
    factory_level_t l = FACTORY_NONE;
    CHECK(factory_level_parse("settings", &l) && l == S);
    CHECK(factory_level_parse("all", &l) && l == A);
    CHECK(!factory_level_parse("everything", &l));
    CHECK(!factory_level_parse("", &l));
}

// ---- the combo ----

static factory_combo_t held_and_armed(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 500) == FC_EV_NONE);
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 600) == FC_EV_NONE);
    CHECK(c.state == FC_HOLDING);
    CHECK(factory_combo_tick(&c, 3500) == FC_EV_NONE);
    CHECK(factory_combo_tick(&c, 3600) == FC_EV_ARM);
    CHECK(factory_combo_swallows(&c));
    return c;
}

static void hold_three_seconds_then_enter(void) {
    factory_combo_t c = held_and_armed();
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_A, false, 3700) == FC_EV_NONE);
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_B, false, 3700) == FC_EV_NONE);
    CHECK(factory_combo_key(&c, FACTORY_COMBO_ENTER, true, 4000) == FC_EV_NONE); // too soon after the question
    CHECK(factory_combo_key(&c, FACTORY_COMBO_ENTER, true, 4700) == FC_EV_CONFIRM);
    CHECK(c.state == FC_OVER && !factory_combo_swallows(&c));
}

static void esc_or_timeout_cancels(void) {
    factory_combo_t c = held_and_armed();
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 5000) == FC_EV_CANCEL);
    c = held_and_armed();
    CHECK(factory_combo_key(&c, 0x20, true, 5000) == FC_EV_NONE); // other keys do nothing
    CHECK(factory_combo_tick(&c, 3600 + FACTORY_COMBO_CONFIRM_MS) == FC_EV_CANCEL);
    CHECK(c.state == FC_OVER);
}

static void short_hold_or_release_resets(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 0);
    factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 100);
    CHECK(factory_combo_key(&c, FACTORY_COMBO_KEY_B, false, 2000) == FC_EV_NONE);
    CHECK(c.state == FC_IDLE);
    CHECK(factory_combo_tick(&c, 5000) == FC_EV_NONE); // released: never arms
    factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 5100); // again: the hold starts over
    CHECK(factory_combo_tick(&c, 8000) == FC_EV_NONE);
    CHECK(factory_combo_tick(&c, 8100) == FC_EV_ARM);
}

static void third_key_breaks_it(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 0);
    factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 0);
    factory_combo_key(&c, 0x2a, true, 1000);
    CHECK(c.state == FC_IDLE);
    CHECK(factory_combo_tick(&c, 4000) == FC_EV_NONE);
}

static void one_key_never_arms(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 0);
    CHECK(factory_combo_tick(&c, 9000) == FC_EV_NONE);
    CHECK(c.state == FC_IDLE);
}

static void window_closes(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    CHECK(factory_combo_tick(&c, FACTORY_COMBO_WINDOW_MS) == FC_EV_NONE);
    CHECK(c.state == FC_OVER);
    factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, FACTORY_COMBO_WINDOW_MS + 1);
    factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, FACTORY_COMBO_WINDOW_MS + 1);
    CHECK(factory_combo_tick(&c, FACTORY_COMBO_WINDOW_MS + 5000) == FC_EV_NONE);
    // a hold that started inside the window still completes after it
    factory_combo_init(&c);
    factory_combo_key(&c, FACTORY_COMBO_KEY_A, true, 9000);
    factory_combo_key(&c, FACTORY_COMBO_KEY_B, true, 9500);
    CHECK(factory_combo_tick(&c, 12500) == FC_EV_ARM);
}

// #391: hand-only = TCA8418 only. "fw keys press esc hold:3500 bksp hold:3500" must not open the question, and an
// injected Enter must not confirm one a hand opened
static void injected_keys_never_arm(void) {
    factory_combo_t c;
    factory_combo_init(&c);
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_A, true, true, 500) == FC_EV_NONE);
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_B, true, true, 600) == FC_EV_NONE);
    CHECK(c.state == FC_IDLE && !c.a_down && !c.b_down);
    CHECK(factory_combo_tick(&c, 4000) == FC_EV_NONE && c.state == FC_IDLE);
    // half by hand, half injected: still no hold
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_A, true, false, 4100) == FC_EV_NONE);
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_B, true, true, 4200) == FC_EV_NONE);
    CHECK(c.state == FC_IDLE && factory_combo_tick(&c, 8000) == FC_EV_NONE);
    // an injected release cannot end a hand's hold either, nor an injected third key break it
    factory_combo_init(&c);
    factory_combo_event(&c, FACTORY_COMBO_KEY_A, true, false, 0);
    factory_combo_event(&c, FACTORY_COMBO_KEY_B, true, false, 100);
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_B, false, true, 1000) == FC_EV_NONE);
    CHECK(factory_combo_event(&c, 0x2a, true, true, 1100) == FC_EV_NONE);
    CHECK(c.state == FC_HOLDING && factory_combo_tick(&c, 3100) == FC_EV_ARM);
}

static void injected_enter_never_confirms(void) {
    factory_combo_t c = held_and_armed();
    CHECK(factory_combo_event(&c, FACTORY_COMBO_ENTER, true, true, 4700) == FC_EV_NONE);
    CHECK(c.state == FC_ARMED && factory_combo_swallows(&c));
    CHECK(factory_combo_event(&c, FACTORY_COMBO_ENTER, true, false, 4800) == FC_EV_CONFIRM); // the hand's Enter does
    // an injected Esc still cancels (the safe way), and the question still times out under injected keys
    c = held_and_armed();
    CHECK(factory_combo_event(&c, FACTORY_COMBO_KEY_A, true, true, 5000) == FC_EV_CANCEL && c.state == FC_OVER);
    c = held_and_armed();
    CHECK(factory_combo_event(&c, FACTORY_COMBO_ENTER, true, true, 3600 + FACTORY_COMBO_CONFIRM_MS) == FC_EV_CANCEL);
    // the hand path through factory_combo_event is factory_combo_key's
    factory_combo_init(&c);
    factory_combo_event(&c, FACTORY_COMBO_KEY_A, true, false, 500);
    factory_combo_event(&c, FACTORY_COMBO_KEY_B, true, false, 600);
    CHECK(c.state == FC_HOLDING);
}

int main(void) {
    app_declared_keys_are_kept();
    boot_plan_per_pending_value();
    settings_go_at_both_levels();
    badge_state_stays();
    identity_only_at_all();
    wifi_wipe_order_survives();
    other_namespaces_stay();
    badgevms_leftovers_go();
    storage_keeps_os_files();
    level_words();
    md5_files_go_back_last();
    storage_erase_fails_then_retries_then_gives_up();
    nvs_flag_write_fails_continues_in_this_boot();
    hold_three_seconds_then_enter();
    esc_or_timeout_cancels();
    short_hold_or_release_resets();
    third_key_breaks_it();
    one_key_never_arms();
    window_closes();
    injected_keys_never_arm();
    injected_enter_never_confirms();
    if (fails) {
        printf("factory_plan_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("factory_plan_test: OK\n");
    return 0;
}
