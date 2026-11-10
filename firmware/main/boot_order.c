#include "boot_order.h"

// The order app_main runs (#210). Moving a line here moves the boot; host_tests/boot_order_test.c holds the rules
// (docs/architecture.md, "Start order (main.c)").
boot_start_t const boot_order[] = {
    START_LOGS,
    START_PSRAM,
    START_NVS,
    START_EARLY,
    START_BOOT_SCREEN,
    START_CONSOLE,
    START_STORAGE,
    START_NOTICES,
    START_FONTS,
    START_SHELL,
    START_WATCH,          // after the shell (its hooks), never inside it: a badge without a display is watched too
    START_SD,             // before c6link: esp_hosted adds SDMMC slot 1 to the controller the card shares
    START_RADIO_SWITCHES, // before Wi-Fi and LoRa start
    START_LORA_IDENT,     // before c6link_start asks the C6 app who it is
    START_C6LINK,
    START_BACKLIGHT,      // the C6 drives the backlights
    START_KEYBOARD,       // the TCA8418 resets with the C6 (#234)
    START_SENSORS,        // the keyboard's bus
    START_LORA,
    START_C6_HOOKS,       // after keyboard and LoRa: from now on a C6 reset is recovered from
    START_APPS,
    START_OTA,
};

size_t const boot_order_len = sizeof(boot_order) / sizeof(boot_order[0]);

int boot_order_index(boot_start_t step) {
    for (size_t i = 0; i < boot_order_len; i++) {
        if (boot_order[i] == step) {
            return (int)i;
        }
    }
    return -1;
}

char const *boot_start_name(boot_start_t step) {
    static char const *const names[START_COUNT] = {
        [START_LOGS] = "logs",           [START_PSRAM] = "psram",       [START_NVS] = "nvs",
        [START_EARLY] = "early",         [START_BOOT_SCREEN] = "boot screen", [START_CONSOLE] = "console",
        [START_STORAGE] = "storage",     [START_NOTICES] = "notices",   [START_FONTS] = "fonts",
        [START_SHELL] = "shell",         [START_WATCH] = "watch",         [START_SD] = "sd",             [START_RADIO_SWITCHES] = "radio switches",
        [START_LORA_IDENT] = "lora ident", [START_C6LINK] = "c6link",   [START_BACKLIGHT] = "backlight",
        [START_KEYBOARD] = "keyboard",   [START_SENSORS] = "sensors",   [START_LORA] = "lora",
        [START_C6_HOOKS] = "c6 hooks",   [START_APPS] = "apps",         [START_OTA] = "ota",
    };
    if ((unsigned)step >= START_COUNT || !names[step]) {
        return "?";
    }
    return names[step];
}
