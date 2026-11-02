// Settings > Factory reset (#66): two levels, each behind careful dialogs (the countdown cancels, Enter counts only
// after a second). "Reset settings" asks once, "Reset everything" twice, the second time about the MeshCore identity.
// A yes saves the reset as pending (factory_reset.h) and restarts the way Power > Restart does, so the apps save and
// stop first; the next boot erases.
#include "esp_log.h"
#include "factory_reset.h"
#include "menu.h"
#include "shell_parts.h"

#define SCREEN_FACTORY 170
#define ASK_SECONDS    15

static char const TAG[] = "factory";

static void failed_dialog(void) {
    shell_dialog_t const d = {
        .title        = "Could not save the reset. Nothing was erased.",
        .count_prefix = "(closes in ",
        .count_suffix = " s)",
        .hint         = "Enter: close",
        .seconds      = 5,
    };
    shell_dialog_countdown(&d, NULL);
}

static void go(factory_level_t level) {
    if (!factory_reset_request(level)) {
        ESP_LOGE(TAG, "reset (%s) not saved", factory_level_name(level));
        failed_dialog();
        return;
    }
    power_request_restart();
}

static void settings_answer(bool yes) {
    if (yes) {
        go(FACTORY_SETTINGS);
    }
}

static void all_second_answer(bool yes) {
    if (yes) {
        go(FACTORY_ALL);
    }
}

static void all_first_answer(bool yes) {
    if (!yes) {
        return;
    }
    shell_dialog_t const d = {
        .title        = "Sure? The MeshCore identity is erased from flash for good: nobody's contacts will know this badge.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: erase everything \xE2\x80\xA2 Esc: cancel",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    shell_dialog_countdown(&d, all_second_answer);
}

static void ask_settings(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title        = "Reset all settings and restart? MeshCore and files stay.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: reset settings \xE2\x80\xA2 Esc: cancel",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    shell_dialog_countdown(&d, settings_answer);
}

static void ask_all(void *arg) {
    (void)arg;
    shell_dialog_t const d = {
        .title        = "Erase everything from flash? Settings and every file on the badge.",
        .count_prefix = "(cancels in ",
        .count_suffix = " s)",
        .hint         = "Enter: continue \xE2\x80\xA2 Esc: cancel",
        .seconds      = ASK_SECONDS,
        .careful      = true,
    };
    shell_dialog_countdown(&d, all_first_answer);
}

static lv_obj_t *build_factory(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Factory reset");
    menu_add(menu, "Reset settings", "", ask_settings, NULL);
    menu_note(menu, "Erases Wi-Fi, time zone, display, radio switches, notification and app settings, then restarts "
                    "into first-run setup. MeshCore keeps its identity, contacts and channels.");
    menu_add(menu, "Reset everything", "", ask_all, NULL);
    menu_note(menu, "Also erases every file on the badge from flash, not just deletes it: the MeshCore identity (for "
                    "good), contacts, channels, messages and the update token. The restart takes up to a minute with "
                    "the screen dark. The SD card is not touched.");
    return screen;
}

void factory_screen_open(void *arg) {
    (void)arg;
    shell_open(build_factory, SCREEN_FACTORY);
}
