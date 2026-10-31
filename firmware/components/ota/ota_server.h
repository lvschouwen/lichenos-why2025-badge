#pragma once

#include <stdbool.h>
#include <stdint.h>

// Wi-Fi update server (#61): tools/badge.sh ota-* on port 8080, guarded by the token in /storage/ota_token.txt
// ("fw token" prints it). Starts listening once Wi-Fi has an address.
void ota_server_start(void);

// Hooks for a screen that shows what an update is doing (#76). They run on the OTA task (core 0): store numbers, never
// block, never call LVGL without its lock. Any member may be NULL.
// begin: a transfer starts (name is the file's name, NULL for firmware); total is the bytes the sender will send, 0 if
// unknown. progress: bytes received so far, counted the same way as total (compressed bytes for a compressed body).
// end: the transfer is over; rebooting is true when the badge is about to restart into the new firmware.
// ASSETS (#22) is handled like FIRMWARE (allowed after /prepare, everything stopped) but is not a new version
typedef enum { OTA_KIND_FIRMWARE, OTA_KIND_FILE, OTA_KIND_ASSETS } ota_kind_t;
typedef struct {
    void (*begin)(ota_kind_t kind, char const *name, uint32_t total);
    void (*progress)(uint32_t done);
    void (*end)(bool ok, bool rebooting);
    // POST /prepare (#76): runs on the OTA task and may block for seconds: stops everything not needed for a firmware
    // update and returns when it has. NULL = the receiver doesn't offer /prepare.
    void (*prepare)(void);
    // false while a firmware update has everything stopped: a file push is then answered 409. NULL = always true.
    bool (*file_ok)(void);
} ota_hooks_t;
void ota_server_set_hooks(ota_hooks_t const *hooks); // copied; before ota_server_start
