#pragma once

// Capabilities (#229): what an app declares it uses, an accident and audit guard (apps are compiled in and share the
// address space, so this is no security boundary). The list and each app's set are declared in tools/layers.conf and
// mirrored here (lichen_app.h includes it) and in lichen_app_t.caps; the build fails when they differ
// (tools/check_layers.py rule D), and rule D checks every symbol an app archive uses against tools/gate_manifest.txt.
// Pure: the host tests and app_caps.c include it without LVGL.
// BACKGROUND is about tasks, not about running off screen (a Timer's lv_timer does that without it): safe mode (#232)
// and the Settings > Apps background switch (#233) key on lichen_app_t.start_background, not on this capability.
#define CAP_LORA_TX     (1u << 0)  // send on LoRa (lora_send, lora_set_tx_power)
#define CAP_LORA_RX     (1u << 1)  // own the LoRa radio: claim it, set it up, take its packets (MeshCore)
#define CAP_LORA_BORROW (1u << 2)  // borrow the radio from its user (lichen_radio_lora_pause, lora_sweep)
#define CAP_SD          (1u << 3)  // the SD card: the app's card directory and write batches (lichen_fs_sd_*)
#define CAP_NET         (1u << 4)  // the network (none for apps today; Wi-Fi is the OS's)
#define CAP_SECRETS     (1u << 5)  // a secrets class (lichen_app_t.secret_prefix, app_secret_*); OS secrets stay SYSTEM
#define CAP_RAW_KEYS    (1u << 6)  // the keyboard below LVGL (lichen_keys.h)
#define CAP_CONSOLE     (1u << 7)  // console commands of its own (console_register)
#define CAP_RESTART     (1u << 8)  // restart the badge (system_restart)
#define CAP_BACKGROUND  (1u << 9)  // may start tasks (lichen_task_start) and may have a start_background hook
#define CAP_HAPTICS     (1u << 10) // the vibration motor (lichen_haptics_play)
#define CAP_SENSORS     (1u << 11) // the environment and motion sensors (lichen_sensors.h)
#define CAP_NOTIFY      (1u << 12) // notifications and the tile badge (lichen_notify.h, lichen_badge_set)
#define CAP_RADIO_SCAN  (1u << 13) // radio scans (#172 scanner, Wi-Fi analyser; none for apps today)
#define CAP_BT          (1u << 14) // Bluetooth (#73; none today)
#define CAP_AUDIO       (1u << 15) // audio out (#67; none today)
#define CAP_BATTERY     (1u << 16) // the battery's state (#11; none for apps today)
#define CAP_SYSTEM      (1u << 17) // the OS's own apps (the shell's): every capability
#define CAP_COUNT       18u

