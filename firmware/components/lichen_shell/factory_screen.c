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
        .title         = "Could not save the reset. Nothing was erased. Try again.",
        .hint          = "Enter: close",
        .seconds       = 5,
        .confirm_label = "close",
    };
    shell_dialog_countdown(&d, NULL);
}

static void go(factory_level_t level) {
    if (!factory_reset_request(level)) {
        ESP_LOGE(TAG, "reset (%s) not saved", factory_level_name(level));
        shell_restart_note(NULL); // nothing will be erased: a later plain restart must not warn of a dark minute
        failed_dialog();
        return;
    }
    power_request_restart();
}

static void ask_settings(void *arg) {
    (void)arg;
    go(FACTORY_SETTINGS);
}

static void all_second_answer(bool yes) {
    if (yes) {
        shell_restart_note("The screen stays dark for up to a minute while everything is erased.");
        go(FACTORY_ALL);
    }
}

static void all_first_answer(bool yes) {
    if (!yes) {
        return;
    }
    shell_dialog_t const d = {
        .title         = "Sure? The MeshCore identity is erased from flash for good: nobody's contacts will know this badge.",
        .seconds       = ASK_SECONDS,
        .careful       = true,
        .confirm_label = "erase everything",
    };
    shell_dialog_countdown(&d, all_second_answer);
}

static void ask_all(void *arg) {
    (void)arg;
    all_first_answer(true); // after the first question, which is the row's DANGER dialog (menu_set_danger)
}

static lv_obj_t *build_factory(void) {
    lv_obj_t *screen = shell_page_create();
    lv_obj_t *menu   = menu_create(screen, "Factory reset");
    lv_obj_t *settings_row = menu_add(menu, "Reset settings", "", ask_settings, NULL);
    menu_set_danger(settings_row, "Reset all settings and restart? MeshCore and files stay.", "reset settings");
    menu_note(menu, "Erases Wi-Fi, time zone, display, radio switches, notification and app settings, then restarts "
                    "into first-run setup. MeshCore keeps its identity, contacts and channels.");
    lv_obj_t *all_row = menu_add(menu, "Reset everything", "", ask_all, NULL);
    menu_set_danger(all_row, "Erase everything from flash? Settings and every file on the badge.", "continue");
    menu_note(menu, "Also erases every file on the badge from flash, not just deletes it: the MeshCore identity (for "
                    "good), contacts, channels, messages and the update token. The restart takes up to a minute with "
                    "the screen dark. The SD card is not touched.");
    return screen;
}

void factory_screen_open(void *arg) {
    (void)arg;
    shell_open(build_factory, SCREEN_FACTORY);
}
