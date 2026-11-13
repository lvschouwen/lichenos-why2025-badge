#include "factory_plan.h"

#include <stddef.h>
#include <string.h>

#define OS_NS "badge_fw" // fw_console.h's FW_NVS_NAMESPACE; this file stays free of ESP headers

// Lichen's keys that describe the badge itself, not the person's choices. A new key in badge_fw is a setting (erased)
// unless it is listed here.
static char const *const keep_keys[] = {
    "boot_fail", "crash_count", "dump_sum", "rst_log", // diagnostics: crashes and restarts stay traceable
    "psram_rty",                                       // PSRAM tuning retries (#40, #86)
    "c6_force", "c6_ok_key", "c6_verify", "c6_rt_restart",                  // the C6's image and recovery state
    "upd_from",                                        // the "updated from" note after an update
    "batt_seen",                                       // battery safety (#10)
    "clock_src",                                       // where the running clock's time came from (#64)
    "r16",                                             // R16 fitted: a hardware record (backlight_r16_hold)
    FACTORY_WIFI_WIPE_KEY,                             // the reset's order to clear the Wi-Fi driver store, until done
};

// App keys that describe the badge's airtime rather than a choice: kept like keep_keys, the rest of the app's
// namespace goes. Declared by the apps (lichen_app_t.keep_keys, #230 E1d) through factory_keep_app_key before the
// reset runs; MeshCore's last advert is one, so the advert interval (#52) holds and a reset loop never floods.
#define KEEP_APP_MAX 8
#define KEEP_NAME_CAP 16 // an NVS namespace or key: 15 characters and the NUL
static char s_keep_app[KEEP_APP_MAX][2][KEEP_NAME_CAP];
static size_t s_keep_app_n;

bool factory_keep_app_key(char const *ns, char const *key) {
    if (!ns || !key || strlen(ns) >= KEEP_NAME_CAP || strlen(key) >= KEEP_NAME_CAP) {
        return false;
    }
    for (size_t i = 0; i < s_keep_app_n; i++) {
        if (!strcmp(s_keep_app[i][0], ns) && !strcmp(s_keep_app[i][1], key)) {
            return true; // declared already
        }
    }
    if (s_keep_app_n >= KEEP_APP_MAX) {
        return false;
    }
    strcpy(s_keep_app[s_keep_app_n][0], ns);
    strcpy(s_keep_app[s_keep_app_n][1], key);
    s_keep_app_n++;
    return true;
}

void factory_keep_app_keys_clear(void) {
    s_keep_app_n = 0;
}

static char const *const keep_files[] = {"c6", "lora.toml"};

factory_boot_t factory_boot_plan(uint8_t pending) {
    factory_boot_t p = {0};
    switch (pending) {
    case FACTORY_SETTINGS:
        p = (factory_boot_t){.nvs = true, .after_nvs = 0, .level = FACTORY_SETTINGS};
        break;
    case FACTORY_ALL:
        p = (factory_boot_t){.nvs = true, .nvs_full = true, .storage = true, .restore = true,
                             .after_nvs = FACTORY_PENDING_STORAGE, .level = FACTORY_ALL};
        break;
    case FACTORY_PENDING_STORAGE:
        p = (factory_boot_t){.storage = true, .restore = true, .level = FACTORY_ALL};
        break;
    case FACTORY_PENDING_ERASING:
        p = (factory_boot_t){.storage = true, .level = FACTORY_ALL};
        break;
    default:
        break;
    }
    return p;
}

static bool listed(char const *const *list, size_t n, char const *s) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i], s) == 0) {
            return true;
        }
    }
    return false;
}

// An app's key goes unless the app declared it kept; its secrets (#288, app_secret.h's '~' mark) go whatever it declared
static bool app_key_erases(char const *ns, char const *key) {
    if (key[0] == '~') {
        return true;
    }
    for (size_t i = 0; i < s_keep_app_n; i++) {
        if (strcmp(ns, s_keep_app[i][0]) == 0 && strcmp(key, s_keep_app[i][1]) == 0) {
            return false;
        }
    }
    return true;
}

bool factory_nvs_erases(char const *ns, char const *key, factory_level_t level) {
    if (!ns || !key || (level != FACTORY_SETTINGS && level != FACTORY_ALL)) {
        return false;
    }
    if (strncmp(ns, "a.", 2) == 0 && ns[2]) {
        return app_key_erases(ns, key); // an app's own settings (lichen_settings.h)
    }
    if (strncmp(ns, "badgevms_", 9) == 0 || strcmp(ns, "backlight") == 0) {
        return true; // left by the factory BadgeVMS firmware (its Wi-Fi password too); nothing in Lichen reads them
    }
    if (strcmp(ns, OS_NS) != 0) {
        return false; // not Lichen's: leave it
    }
    if (strcmp(key, FACTORY_PENDING_KEY) == 0 || strcmp(key, FACTORY_TRIES_KEY) == 0) {
        return false; // factory_reset.c's own state
    }
    return !listed(keep_keys, sizeof(keep_keys) / sizeof(keep_keys[0]), key);
}

bool factory_storage_keeps(char const *name) {
    return name && listed(keep_files, sizeof(keep_files) / sizeof(keep_files[0]), name);
}

factory_storage_next_t factory_storage_after(bool erased, uint8_t tries) {
    if (erased) {
        return (factory_storage_next_t){0};
    }
    uint8_t done = (uint8_t)(tries + 1);
    if (done >= FACTORY_STORAGE_TRIES) {
        return (factory_storage_next_t){.fallback_wipe = true, .notice = true}; // stop: no boot loop of erases
    }
    return (factory_storage_next_t){.fallback_wipe = true, .pending = FACTORY_PENDING_STORAGE, .tries = done};
}

factory_nvs_next_t factory_nvs_after(bool erased, bool flag_saved) {
    return (factory_nvs_next_t){.key_fallback = !erased, .carry_in_ram = !flag_saved};
}

bool factory_restore_late(char const *name) {
    size_t n = name ? strlen(name) : 0;
    return n >= 4 && strcmp(name + n - 4, ".md5") == 0;
}

bool factory_level_parse(char const *word, factory_level_t *out) {
    if (!word) {
        return false;
    }
    if (strcmp(word, "settings") == 0) {
        *out = FACTORY_SETTINGS;
    } else if (strcmp(word, "all") == 0) {
        *out = FACTORY_ALL;
    } else {
        return false;
    }
    return true;
}

char const *factory_level_name(factory_level_t level) {
    return level == FACTORY_SETTINGS ? "settings" : level == FACTORY_ALL ? "all" : "none";
}

// ---- the boot combo ------------------------------------------------------------------------------------------------

void factory_combo_init(factory_combo_t *c) {
    *c = (factory_combo_t){.state = FC_IDLE};
}

fc_event_t factory_combo_tick(factory_combo_t *c, uint32_t now_ms) {
    switch (c->state) {
    case FC_IDLE:
        if (now_ms >= FACTORY_COMBO_WINDOW_MS) {
            c->state = FC_OVER;
        }
        return FC_EV_NONE;
    case FC_HOLDING:
        if (now_ms - c->since >= FACTORY_COMBO_HOLD_MS) {
            c->state = FC_ARMED;
            c->since = now_ms;
            return FC_EV_ARM;
        }
        return FC_EV_NONE;
    case FC_ARMED:
        if (now_ms - c->since >= FACTORY_COMBO_CONFIRM_MS) {
            c->state = FC_OVER;
            return FC_EV_CANCEL;
        }
        return FC_EV_NONE;
    default:
        return FC_EV_NONE;
    }
}

fc_event_t factory_combo_key(factory_combo_t *c, uint8_t code, bool pressed, uint32_t now_ms) {
    fc_event_t ev = factory_combo_tick(c, now_ms);
    if (ev != FC_EV_NONE || c->state == FC_OVER) {
        return ev;
    }
    if (c->state == FC_ARMED) {
        if (!pressed) {
            return FC_EV_NONE;
        }
        if (code == FACTORY_COMBO_KEY_A) { // Esc
            c->state = FC_OVER;
            return FC_EV_CANCEL;
        }
        if (code == FACTORY_COMBO_ENTER && now_ms - c->since >= FACTORY_COMBO_ENTER_MS) {
            c->state = FC_OVER;
            return FC_EV_CONFIRM;
        }
        return FC_EV_NONE;
    }
    if (code == FACTORY_COMBO_KEY_A) {
        c->a_down = pressed;
    } else if (code == FACTORY_COMBO_KEY_B) {
        c->b_down = pressed;
    } else if (pressed) {
        c->state = FC_IDLE; // a third key: not the combo (a hand on the keyboard, a bag)
        return FC_EV_NONE;
    }
    bool both = c->a_down && c->b_down;
    if (c->state == FC_IDLE && both) {
        c->state = FC_HOLDING; // within the window: tick above closed it otherwise
        c->since = now_ms;
    } else if (c->state == FC_HOLDING && !both) {
        c->state = FC_IDLE;
    }
    return FC_EV_NONE;
}

fc_event_t factory_combo_event(factory_combo_t *c, uint8_t code, bool pressed, bool injected, uint32_t now_ms) {
    if (!injected) {
        return factory_combo_key(c, code, pressed, now_ms);
    }
    fc_event_t ev = factory_combo_tick(c, now_ms);
    if (ev == FC_EV_NONE && c->state == FC_ARMED && pressed && code == FACTORY_COMBO_KEY_A) {
        c->state = FC_OVER;
        return FC_EV_CANCEL;
    }
    return ev;
}

bool factory_combo_swallows(factory_combo_t const *c) {
    return c->state == FC_ARMED;
}
