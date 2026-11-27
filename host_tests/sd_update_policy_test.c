// Host tests for the SD card's P4 update rule (#595, D1): what the card file is, and whether this boot installs it
#include "sd_update_policy.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define P4_CHIP 0x0012

// An app image head: 0xE9, the chip at 12, esp_app_desc_t's magic at 0x20 and its ELF sha at 0x20 + 0x90
static void make_head(uint8_t head[SDU_HEAD_LEN], uint8_t fill) {
    memset(head, 0, SDU_HEAD_LEN);
    head[0]    = 0xE9;
    head[12]   = P4_CHIP & 0xff;
    head[13]   = P4_CHIP >> 8;
    head[0x20] = 0x32;
    head[0x21] = 0x54;
    head[0x22] = 0xCD;
    head[0x23] = 0xAB;
    memset(&head[0xB0], fill, SDU_SHA_LEN);
}

static void test_head(void) {
    uint8_t head[SDU_HEAD_LEN], sha[SDU_SHA_LEN];
    make_head(head, 0x5A);
    CHECK(sdu_card_from_head(head, sizeof(head), P4_CHIP, sha) == SDU_CARD_OK);
    CHECK(sha[0] == 0x5A && sha[31] == 0x5A);
    CHECK(sdu_card_from_head(head, sizeof(head) - 1, P4_CHIP, sha) == SDU_CARD_UNREADABLE); // a short read
    CHECK(sdu_card_from_head(NULL, 0, P4_CHIP, sha) == SDU_CARD_UNREADABLE);
    CHECK(sdu_card_from_head(head, sizeof(head), 0x000D, sha) == SDU_CARD_NOT_APP); // a C6 image is no P4 update
    head[0] = 0x00;
    CHECK(sdu_card_from_head(head, sizeof(head), P4_CHIP, sha) == SDU_CARD_NOT_APP); // no image magic
    make_head(head, 0x5A);
    head[0x23] = 0x00;
    CHECK(sdu_card_from_head(head, sizeof(head), P4_CHIP, sha) == SDU_CARD_NOT_APP); // no app descriptor
    make_head(head, 0x00);
    CHECK(sdu_card_from_head(head, sizeof(head), P4_CHIP, sha) == SDU_CARD_NOT_APP); // ELF sha never filled in
}

// Every row of D1: card state x same as running x same as attempted x on trial
static void test_decide_table(void) {
    for (int r = 0; r < 2; r++) {
        for (int a = 0; a < 2; a++) {
            for (int t = 0; t < 2; t++) {
                CHECK(sdu_decide(SDU_CARD_ABSENT, r, a, t) == SDU_SKIP_ABSENT);
                CHECK(sdu_decide(SDU_CARD_UNREADABLE, r, a, t) == SDU_SKIP_UNREADABLE);
                CHECK(sdu_decide(SDU_CARD_NOT_APP, r, a, t) == SDU_SKIP_NOT_APP);
                sdu_action_t want = r ? SDU_SKIP_SAME : a ? SDU_SKIP_TRIED : t ? SDU_SKIP_TRIAL : SDU_INSTALL;
                CHECK(sdu_decide(SDU_CARD_OK, r, a, t) == want);
            }
        }
    }
    // The cases by name
    CHECK(sdu_decide(SDU_CARD_OK, false, false, false) == SDU_INSTALL);   // new and untried
    CHECK(sdu_decide(SDU_CARD_OK, true, true, true) == SDU_SKIP_SAME);    // the boot right after a card install
    CHECK(sdu_decide(SDU_CARD_OK, false, true, false) == SDU_SKIP_TRIED); // rolled back: never loops
    CHECK(sdu_decide(SDU_CARD_OK, false, false, true) == SDU_SKIP_TRIAL); // a Wi-Fi update still on trial
    CHECK(sdu_decide((sdu_card_t)99, false, false, false) == SDU_SKIP_UNREADABLE); // fails closed
}

static void test_notice(void) {
    for (int a = SDU_INSTALL; a <= SDU_SKIP_TRIAL; a++) {
        CHECK(sdu_action_name((sdu_action_t)a)[0] != '?');
        for (int told = 0; told < 2; told++) {
            bool want = a == SDU_SKIP_UNREADABLE || a == SDU_SKIP_NOT_APP || (a == SDU_SKIP_TRIED && !told);
            CHECK(sdu_action_notice((sdu_action_t)a, told) == want);
        }
    }
    CHECK(sdu_action_name((sdu_action_t)99)[0] == '?');
}

static void test_sha_text(void) {
    uint8_t d[SDU_SHA_LEN];
    for (int i = 0; i < SDU_SHA_LEN; i++) {
        d[i] = (uint8_t)(0xA0 + i);
    }
    char hex[65];
    for (int i = 0; i < SDU_SHA_LEN; i++) {
        snprintf(hex + 2 * i, 3, "%02x", d[i]);
    }
    char buf[128];
    CHECK(sdu_sha256_text_ok(hex, 64, d));
    snprintf(buf, sizeof(buf), "%s  badge_fw.bin\n", hex); // sha256sum's own line
    CHECK(sdu_sha256_text_ok(buf, strlen(buf), d));
    snprintf(buf, sizeof(buf), "%s\n", hex);
    CHECK(sdu_sha256_text_ok(buf, strlen(buf), d));
    snprintf(buf, sizeof(buf), "%s\r\n", hex);
    CHECK(sdu_sha256_text_ok(buf, strlen(buf), d));
    for (int i = 0; buf[i]; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'f') {
            buf[i] = (char)(buf[i] - 'a' + 'A');
        }
    }
    CHECK(sdu_sha256_text_ok(buf, strlen(buf), d)); // either case
    snprintf(buf, sizeof(buf), "%s0", hex);
    CHECK(!sdu_sha256_text_ok(buf, strlen(buf), d)); // 65 digits is another hash
    CHECK(!sdu_sha256_text_ok(hex, 63, d));          // short
    CHECK(!sdu_sha256_text_ok(NULL, 64, d));
    CHECK(!sdu_sha256_text_ok("", 0, d));
    d[31] ^= 1;
    CHECK(!sdu_sha256_text_ok(hex, 64, d)); // another file
    snprintf(buf, sizeof(buf), "g%s", hex + 1);
    CHECK(!sdu_sha256_text_ok(buf, 64, d));
}

// #595 part B: an image that matches its .sha256 but fails after the copy is recorded, so it is written once only;
// a failure that is not the image's (the card, the flash, the boot slot) is tried again at the next boot
static void test_result_records(void) {
    for (int r = SDU_DONE; r <= SDU_FAIL_BOOT; r++) {
        bool want = r == SDU_FAIL_IMAGE || r == SDU_FAIL_READBACK;
        CHECK(sdu_result_records((sdu_result_t)r) == want);
        CHECK(sdu_result_text((sdu_result_t)r)[0] != '?');
    }
    CHECK(!sdu_result_records(SDU_FAIL_READ)); // a card pulled mid-copy: tried again once it is back
    CHECK(!sdu_result_records(SDU_FAIL_SHA));  // checked before anything is erased
    CHECK(sdu_result_text((sdu_result_t)99)[0] == '?');
    // The next boot with the same file: recorded as tried, and told already, so no second notice
    CHECK(sdu_decide(SDU_CARD_OK, false, true, false) == SDU_SKIP_TRIED);
    CHECK(!sdu_action_notice(SDU_SKIP_TRIED, (SDU_REC_FAILED | SDU_REC_TOLD) & SDU_REC_TOLD));
    // The two kinds of tried say different things
    CHECK(strcmp(sdu_tried_text(SDU_REC_FAILED), sdu_tried_text(0)) != 0);
    CHECK(strcmp(sdu_tried_text(SDU_REC_FAILED | SDU_REC_TOLD), sdu_tried_text(SDU_REC_FAILED)) == 0);
}

// Review M3 and M4: the trial rule by provenance, and the boot screen's row after the step
static void test_trial_and_step(void) {
    CHECK(sdu_step_of(SDU_SKIP_ABSENT, SDU_DONE) == SDU_STEP_SKIPPED);
    CHECK(sdu_step_of(SDU_SKIP_SAME, SDU_DONE) == SDU_STEP_DONE);
    CHECK(sdu_step_of(SDU_SKIP_TRIED, SDU_DONE) == SDU_STEP_SKIPPED);
    CHECK(sdu_step_of(SDU_SKIP_TRIAL, SDU_DONE) == SDU_STEP_SKIPPED);
    CHECK(sdu_step_of(SDU_SKIP_UNREADABLE, SDU_DONE) == SDU_STEP_FAILED);
    CHECK(sdu_step_of(SDU_SKIP_NOT_APP, SDU_DONE) == SDU_STEP_FAILED);
    CHECK(sdu_step_of(SDU_INSTALL, SDU_DONE) == SDU_STEP_DONE);
    for (int r = SDU_FAIL_READ; r <= SDU_FAIL_BOOT; r++) {
        CHECK(sdu_step_of(SDU_INSTALL, (sdu_result_t)r) == SDU_STEP_FAILED);
        CHECK(sdu_step_of(SDU_SKIP_SAME, (sdu_result_t)r) == SDU_STEP_DONE); // r only counts for an install
    }
    CHECK(sdu_step_of((sdu_action_t)99, SDU_DONE) == SDU_STEP_SKIPPED);
}

// Review H1: the Wi-Fi provenance is the pushed image's SHA-256 in the attempt record, not a file on the card. A Wi-Fi
// delete of anything in the folder changes no record, so the pushed image keeps the Wi-Fi trial; a card prepared by
// hand with another image gets the no-Wi-Fi trial
static void test_wifi_provenance(void) {
    uint8_t pushed[SDU_SHA_LEN], hand[SDU_SHA_LEN], none[SDU_SHA_LEN] = {0};
    memset(pushed, 0xA5, sizeof(pushed));
    memset(hand, 0xA5, sizeof(hand));
    hand[31] ^= 1;
    uint8_t rec[SDU_SHA_LEN];
    memcpy(rec, pushed, sizeof(rec)); // ota_sd_update_note_wifi_image before the pushed file landed
    CHECK(!sdu_trial_without_wifi(pushed, rec)); // the pushed image: the Wi-Fi rule
    // ... after any Wi-Fi delete in the folder (a marker-like name, the .sha256, badge_fw.bin itself pushed again): the
    // record is untouched by deletes, so the same image still gets the Wi-Fi rule
    CHECK(!sdu_trial_without_wifi(pushed, rec));
    CHECK(sdu_trial_without_wifi(hand, rec));  // a different image copied on a PC: physical access
    CHECK(sdu_trial_without_wifi(hand, none)); // nothing ever pushed over Wi-Fi
    CHECK(sdu_trial_without_wifi(none, none)); // an all-zero record means none, never "matches"
    // The record's flags: the attempt starts untold, a failed image is recorded failed and told, else unchanged
    CHECK(sdu_flags_after(SDU_REC_TOLD, SDU_DONE) == 0);
    CHECK(sdu_flags_after(0, SDU_FAIL_IMAGE) == (SDU_REC_FAILED | SDU_REC_TOLD));
    CHECK(sdu_flags_after(0, SDU_FAIL_READBACK) == (SDU_REC_FAILED | SDU_REC_TOLD));
    CHECK(sdu_flags_after(SDU_REC_TOLD, SDU_FAIL_READ) == SDU_REC_TOLD);
    CHECK(SDU_READBACK_TRIES == 2); // L1: one more read before a read-back mismatch is recorded
    // The hex the server has (X-File-SHA256, lowercase from the receiver) to the bytes the record holds
    char    hex[65];
    uint8_t back[SDU_SHA_LEN];
    for (int i = 0; i < SDU_SHA_LEN; i++) {
        snprintf(hex + 2 * i, 3, "%02x", hand[i]);
    }
    CHECK(sdu_sha_from_hex(hex, back) && memcmp(back, hand, SDU_SHA_LEN) == 0);
    hex[63] = 'g';
    CHECK(!sdu_sha_from_hex(hex, back));
    hex[63] = '\0';
    CHECK(!sdu_sha_from_hex(hex, back)); // 63 digits
    CHECK(!sdu_sha_from_hex(NULL, back));
}

int main(void) {
    test_head();
    test_decide_table();
    test_notice();
    test_sha_text();
    test_result_records();
    test_trial_and_step();
    test_wifi_provenance();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_update_policy_test ok\n");
    return 0;
}
