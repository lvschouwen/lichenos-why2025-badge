// The P4 image in the SD card's update folder (#595): installed at boot by itself when it differs from the running one
// and was not tried before (sd_update_policy.h decides). The C6 bundle next to it is c6link's (c6_card.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OTA_SD_UPDATE_DIR "/sd/lichen/update" // badge_fw.bin + badge_fw.bin.sha256, and c6/ (tools/badge.sh sd-prep)

// The attempt record (the rollback guard of D1): the ELF sha256 of the image the last card install made the boot slot,
// and whether the "tried before" notice for it was raised. ota may not open NVS (#235): main binds the settings' blob.
typedef struct {
    uint8_t elf_sha[32]; // all zero: none (an image without an ELF sha is never installed)
    uint8_t flags;       // SDU_REC_TOLD, SDU_REC_FAILED (sd_update_policy.h)
} ota_sd_record_t;

typedef struct {
    // A notice for the person (a file ignored, an install that failed). Default: the log only
    void (*notice)(bool warn, char const *title, char const *body);
    // The boot screen's update row (BOOT_UPDATE): begin, the install's percent, end (sdu_step_t, a short note).
    // Default: not shown
    void (*step_begin)(void);
    void (*step_progress)(int percent);
    void (*step_end)(int step, char const *note);
    // The record's store; get false: none stored. Without both no install runs (no guard, no install).
    bool (*record_get)(ota_sd_record_t *out);
    bool (*record_set)(ota_sd_record_t const *rec);
} ota_sd_update_hooks_t;

// The boot step START_SD_UPDATE, after the C6 step (the C6 first, then the P4) and before the apps. Card I/O under the
// card's batch lock. An install writes the inactive slot, reads it back, records the image as attempted, makes it the
// boot slot and restarts into it (no return). Otherwise it returns: nothing on the card, the same image, a skip with a
// reason, or a failure that left the boot slot alone. Its trial needs no Wi-Fi (a card is physical access, like
// `fw slot other`); a bad image still rolls back, and the attempt record keeps it from being installed again.
void ota_sd_update_boot(ota_sd_update_hooks_t const *hooks);

// Review M3: the update server is about to write or delete under OTA_SD_UPDATE_DIR over Wi-Fi: leave the Wi-Fi marker
// first (SDU_WIFI_MARK), so the next install from there keeps the Wi-Fi trial rule. Takes the card's batch lock (not
// held by the caller). False: not written, so the change must not go ahead.
bool ota_sd_update_mark_wifi(void);
