#pragma once

#include <stddef.h>

// The start-up order of app_main (#210), as data: main.c walks boot_order[] and calls one function per entry. The
// order is part of the P4 <-> C6 contract (docs/architecture.md, "Start order (main.c)"), pinned by
// host_tests/boot_order_test.c. ESP-free on purpose, so the host test sees the same table the badge runs.
//
// Not to be confused with boot_steps.h: that enum is the display order of the "fw boot" rows and the boot screen.

typedef enum {
    START_LOGS,           // entropy, log tap, PSRAM canaries, log store, boot progress, the version and heap lines
    START_PSRAM,          // the PSRAM row
    START_NVS,            // NVS, a pending factory reset's settings part, the PSRAM speed record and one-shot
    START_EARLY,          // haptics, time zone, crash_check_early: cheap, before the first frame
    START_BOOT_SCREEN,    // display, the boot screen's first frame, backlight_restore (#39)
    START_CONSOLE,        // the serial console
    START_STORAGE,        // a factory reset's /storage part, then the FAT mount
    START_NOTICES,        // notifications, LED, crash_check_late, console commands that need storage
    START_FONTS,          // assets into PSRAM fonts
    START_SHELL,          // MeshCore registered, the restart save, the shell when there is a screen
    START_SD,             // the SD card (#60)
    START_RADIO_SWITCHES, // the saved OS radio switches (#108)
    START_LORA_IDENT,     // c6link's IDENT hooks from lora_ident (#36)
    START_C6LINK,         // c6link_start: esp_hosted, the C6 image check, Wi-Fi
    START_BACKLIGHT,      // backlight_start (the C6 drives the backlights)
    START_KEYBOARD,       // the TCA8418 (reset with the C6, #14)
    START_SENSORS,        // on the keyboard's bus
    START_LORA,           // lora_start, only with the C6 link up
    START_C6_HOOKS,       // the C6 reset recovery hooks (#14 T-B)
    START_APPS,           // every app's background part (#66)
    START_OTA,            // the OTA server and its hooks (#76)
    START_COUNT
} boot_start_t;

extern boot_start_t const boot_order[];
extern size_t const       boot_order_len;

// Where step sits in boot_order[], or -1 when it is missing
int         boot_order_index(boot_start_t step);
char const *boot_start_name(boot_start_t step);
