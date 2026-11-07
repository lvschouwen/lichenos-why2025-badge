// Host test: the P4 <-> C6 contract of docs/architecture.md "Two-chip contract" (#234). It pins what both images must
// keep agreeing on: the peer-data message ids, the wire bytes of CONFIG and the IDENT compare. The start order is not
// testable here (it lives in main.c): see the doc; #210 pins it with a boot-step table test. A failure here means one
// chip would stop understanding the other: change both images and the doc together.
#include "c6_boot_policy.h"
#include "c6_paths.h"
#include "i154_proto.h"
#include "lora_proto.h"

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

// Every peer-data message id of a P4 <-> C6 service (the i154 ids are reserved, not wired yet, #172). A new service adds a row here and a line in the doc; a duplicate
// would route one service's messages into the other's callback.
typedef struct {
    int         id;
    char const *name;
} peer_id_t;

static const peer_id_t peer_ids[] = {
    {LORA_MSG_ID_TO_C6, "lora to C6"},
    {LORA_MSG_ID_TO_P4, "lora to P4"},
    {I154_MSG_ID_TO_C6, "i154 to C6"},
    {I154_MSG_ID_TO_P4, "i154 to P4"},
};
#define PEER_ID_COUNT (sizeof(peer_ids) / sizeof(peer_ids[0]))

static void test_message_ids(void) {
    for (size_t i = 0; i < PEER_ID_COUNT; i++) {
        CHECK(peer_ids[i].id > 0 && peer_ids[i].id <= 0xFFFF); // the peer-data id is 16 bits wide
        for (size_t j = i + 1; j < PEER_ID_COUNT; j++) {
            if (peer_ids[i].id == peer_ids[j].id) {
                printf("duplicate peer-data id 0x%04X: %s / %s\n", peer_ids[i].id, peer_ids[i].name, peer_ids[j].name);
            }
            CHECK(peer_ids[i].id != peer_ids[j].id);
        }
    }
    // The values are wire format and quoted by the doc
    CHECK(LORA_MSG_ID_TO_C6 == 0x4C01 && LORA_MSG_ID_TO_P4 == 0x4C02);
    CHECK(I154_MSG_ID_TO_C6 == 0x4C03 && I154_MSG_ID_TO_P4 == 0x4C04);
    CHECK(LORA_PROTO_VERSION == 4 && I154_PROTO_VERSION == 1);
}

// P4 -> C6 types count up from 1, C6 -> P4 types from 0x81, in the order the doc lists
static void test_type_tables(void) {
    int const lora_to_c6[] = {LORA_MSG_CONFIG,   LORA_MSG_RX_START, LORA_MSG_RX_STOP, LORA_MSG_STATUS_REQ,
                              LORA_MSG_SEND,     LORA_MSG_IDENT_REQ, LORA_MSG_SWEEP_REQ};
    int const lora_to_p4[] = {LORA_MSG_RX_PACKET, LORA_MSG_STATUS, LORA_MSG_RESULT, LORA_MSG_IDENT, LORA_MSG_SWEEP};
    int const i154_to_c6[] = {I154_MSG_CAPS_REQ, I154_MSG_SCAN_START, I154_MSG_SCAN_STOP};
    int const i154_to_p4[] = {I154_MSG_CAPS, I154_MSG_ED, I154_MSG_FRAME, I154_MSG_DONE, I154_MSG_ACK};
    for (int i = 0; i < (int)(sizeof(lora_to_c6) / sizeof(int)); i++) {
        CHECK(lora_to_c6[i] == i + 1);
    }
    for (int i = 0; i < (int)(sizeof(lora_to_p4) / sizeof(int)); i++) {
        CHECK(lora_to_p4[i] == 0x81 + i);
    }
    for (int i = 0; i < (int)(sizeof(i154_to_c6) / sizeof(int)); i++) {
        CHECK(i154_to_c6[i] == i + 1);
    }
    for (int i = 0; i < (int)(sizeof(i154_to_p4) / sizeof(int)); i++) {
        CHECK(i154_to_p4[i] == 0x81 + i);
    }
}

// A message of one service never decodes as the other's: a mixed-up id would at worst be dropped
static void test_services_do_not_decode_each_other(void) {
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t lm  = {.type = LORA_MSG_CONFIG, .u.config = LORA_CONFIG_DEFAULT};
    size_t     n   = lora_encode(&lm, buf, sizeof(buf));
    i154_msg_t out = {0};
    CHECK(n > 0 && i154_decode(buf, n, &out) != I154_DEC_OK); // lora v4 against i154 v1: BAD_VERSION

    i154_msg_t im = {.type = I154_MSG_CAPS_REQ};
    n             = i154_encode(&im, buf, sizeof(buf));
    lora_msg_t lout;
    CHECK(n > 0 && lora_decode(buf, n, &lout) != LORA_DEC_OK);
}

// CONFIG wire layout: [version][type] freq u32, bw u32, sf, cr, sync, preamble u16, tx power i8, little-endian. The C6's
// radio is configured from exactly these 16 bytes, so the default is pinned byte for byte.
static void test_config_layout(void) {
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t cfg  = {.type = LORA_MSG_CONFIG, .u.config = LORA_CONFIG_DEFAULT};
    size_t     n    = lora_encode(&cfg, buf, sizeof(buf));
    uint32_t   freq = 869618000u, bw = 62500u;
    uint8_t    want[16] = {LORA_PROTO_VERSION,
                           LORA_MSG_CONFIG,
                           (uint8_t)freq,
                           (uint8_t)(freq >> 8),
                           (uint8_t)(freq >> 16),
                           (uint8_t)(freq >> 24),
                           (uint8_t)bw,
                           (uint8_t)(bw >> 8),
                           (uint8_t)(bw >> 16),
                           (uint8_t)(bw >> 24),
                           7,    // sf
                           5,    // cr 4/5
                           0x12, // sync word
                           16,   // preamble low
                           0,    // preamble high
                           22};  // dBm
    CHECK(n == sizeof(want));
    CHECK(memcmp(buf, want, sizeof(want)) == 0);
    CHECK(LORA_MSG_MAX >= 266); // an RX_PACKET with 255 bytes must fit the peer-data buffers on both sides

    // A negative power survives as a two's complement byte
    cfg.u.config.tx_power_dbm = -9;
    n                         = lora_encode(&cfg, buf, sizeof(buf));
    CHECK(n == 16 && buf[15] == 0xF7);
    lora_msg_t out;
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.config.tx_power_dbm == -9);

    CHECK(lora_decode(buf, 15, &out) == LORA_DEC_BAD_LEN);
    CHECK(lora_decode(buf, 17, &out) == LORA_DEC_BAD_LEN);
    lora_config_t def = LORA_CONFIG_DEFAULT;
    CHECK(lora_config_valid(&def));
}

// IDENT: 32 bytes, compared whole. One flipped bit anywhere is a mismatch, so a stale C6 app is never taken for the bundle
static void test_ident_compare(void) {
    CHECK(C6_IDENT_SHA_LEN == LORA_IDENT_SHA_LEN);
    uint8_t a[C6_IDENT_SHA_LEN] = {0}, b[C6_IDENT_SHA_LEN] = {0};
    for (int i = 0; i < C6_IDENT_SHA_LEN; i++) {
        a[i] = (uint8_t)(i * 5 + 1);
    }
    memcpy(b, a, sizeof(a));
    CHECK(c6_ident_check(a, b) == C6_IDENT_MATCH);
    for (int i = 0; i < C6_IDENT_SHA_LEN; i++) {
        for (int bit = 0; bit < 8; bit++) {
            b[i] ^= (uint8_t)(1u << bit);
            CHECK(c6_ident_check(a, b) == C6_IDENT_MISMATCH);
            b[i] ^= (uint8_t)(1u << bit);
        }
    }

    // The request is two bytes, the answer carries the sha right after the header (a v3 C6 drops both: BAD_VERSION)
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t req = {.type = LORA_MSG_IDENT_REQ};
    CHECK(lora_encode(&req, buf, sizeof(buf)) == 2 && buf[0] == LORA_PROTO_VERSION && buf[1] == LORA_MSG_IDENT_REQ);
    lora_msg_t id = {.type = LORA_MSG_IDENT};
    memcpy(id.u.ident.app_elf_sha256, a, sizeof(a));
    CHECK(lora_encode(&id, buf, sizeof(buf)) == 2 + LORA_IDENT_SHA_LEN && memcmp(buf + 2, a, sizeof(a)) == 0);
}

// The bundled C6 files live in one directory on /storage, and the .md5 sits next to its .bin
static void test_c6_files(void) {
    char out[64];
    CHECK(strcmp(C6_IMAGE_DIR, "/storage/c6") == 0);
    CHECK(c6_image_path("firmware.bin", out, sizeof(out)) && strcmp(out, "/storage/c6/firmware.bin") == 0);
    CHECK(c6_image_path("firmware.bin.md5", out, sizeof(out)) && strcmp(out, "/storage/c6/firmware.bin.md5") == 0);
    CHECK(!c6_image_path("../firmware.bin", out, sizeof(out)));
}

int main(void) {
    test_message_ids();
    test_type_tables();
    test_services_do_not_decode_each_other();
    test_config_layout();
    test_ident_compare();
    test_c6_files();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("two_chip_contract_test: ok\n");
    return 0;
}
