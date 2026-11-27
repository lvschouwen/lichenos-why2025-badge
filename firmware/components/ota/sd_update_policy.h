// The P4 image in the SD card's update folder (#595): whether this boot installs it. Pure, no ESP headers: host-tested
// in host_tests/sd_update_policy_test.c. The glue is ota_sd_update.c.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SDU_SHA_LEN 32

// What the first bytes of the card's badge_fw.bin say
typedef enum {
    SDU_CARD_ABSENT,     // no card mounted, or no badge_fw.bin on it
    SDU_CARD_UNREADABLE, // there, but its first bytes could not be read
    SDU_CARD_NOT_APP,    // not an app image for this chip, or no app descriptor with an ELF sha256
    SDU_CARD_OK,
} sdu_card_t;

// The image header (magic, chip) and the esp_app_desc_t at offset 32: SDU_CARD_OK with the ELF sha256 in elf_sha, else
// UNREADABLE (fewer than SDU_HEAD_LEN bytes) or NOT_APP. Needs the first SDU_HEAD_LEN bytes.
#define SDU_HEAD_LEN 0xD0
sdu_card_t sdu_card_from_head(uint8_t const *head, size_t len, uint16_t chip_id, uint8_t elf_sha[SDU_SHA_LEN]);

typedef enum {
    SDU_INSTALL,
    SDU_SKIP_ABSENT,     // nothing on the card: the usual boot, not worth a word
    SDU_SKIP_UNREADABLE, // a notice: the file is there but cannot be read
    SDU_SKIP_NOT_APP,    // a notice: not a badge image
    SDU_SKIP_SAME,       // the card holds the running image
    SDU_SKIP_TRIED,      // installed from the card before and the badge came back on the old one (rollback guard, D1)
    SDU_SKIP_TRIAL,      // the running image is on trial: esp_ota_begin refuses; the boot after its mark-valid decides
} sdu_action_t;

// D1 of #595: install only an image that differs from the running one and was not tried before. same_as_running: the
// card's ELF sha256 is the running app's; same_as_attempted: it is the one recorded before the last card install;
// on_trial: the running image is definitely on trial (blank OTA data after a USB flash is not, or a USB-flashed badge
// would never install from the card).
sdu_action_t sdu_decide(sdu_card_t card, bool same_as_running, bool same_as_attempted, bool on_trial);
char const  *sdu_action_name(sdu_action_t a);
// Whether a skip raises a notice: an unreadable or foreign file always (the card needs fixing), a tried image only the
// first time (told: that notice was raised since the attempt was recorded), the rest never
bool sdu_action_notice(sdu_action_t a, bool told);

// The text of badge_fw.bin.sha256 (sha256sum's format: 64 hex digits, either case, then the end, white space or the
// file name) names digest
bool sdu_sha256_text_ok(char const *text, size_t len, uint8_t const digest[SDU_SHA_LEN]);
