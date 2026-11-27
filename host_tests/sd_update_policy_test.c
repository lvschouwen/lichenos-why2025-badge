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

int main(void) {
    test_head();
    test_decide_table();
    test_notice();
    test_sha_text();
    test_result_records();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("sd_update_policy_test ok\n");
    return 0;
}
