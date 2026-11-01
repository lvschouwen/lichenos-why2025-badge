// Host test: MeshCore companion frame codec (#45)
#include "mesh_frame.h"

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

static void encoders(void) {
    uint8_t f[MF_MAX_FRAME];
    CHECK(mf_encode_device_query(f, 3) == 2 && f[0] == 22 && f[1] == 3);
    size_t n = mf_encode_app_start(f, "lichen");
    CHECK(n == 14 && f[0] == 1 && memcmp(f + 8, "lichen", 6) == 0);
    CHECK(mf_encode_sync_next(f) == 1 && f[0] == 10);

    n = mf_encode_send_channel(f, 2, 0x01020304, "hi", 2);
    CHECK(n == 9 && f[0] == 3 && f[1] == 0 && f[2] == 2 && f[3] == 4 && f[6] == 1 && memcmp(f + 7, "hi", 2) == 0);

    uint8_t const prefix[6] = {1, 2, 3, 4, 5, 6};
    n = mf_encode_send_dm(f, 1, 0x0A0B0C0D, prefix, "yo", 2);
    CHECK(n == 15 && f[0] == 2 && f[2] == 1 && f[3] == 0x0D && memcmp(f + 7, prefix, 6) == 0 && memcmp(f + 13, "yo", 2) == 0);

    uint8_t pub[32];
    memset(pub, 9, sizeof(pub));
    CHECK(mf_encode_reset_path(f, pub) == 33 && f[0] == 13 && f[32] == 9);
    CHECK(mf_encode_remove_contact(f, pub) == 33 && f[0] == 15 && f[1] == pub[0] && f[32] == pub[31]);

    // CMD_ADD_UPDATE_CONTACT (#49): MyMesh::updateContactFromFrame's offsets, lastmod at the end
    mf_contact_t c = {.type = 1, .flags = 1, .out_path_len = 0x41, .last_advert_timestamp = 0x01020304,
                      .lastmod = 0x0A0B0C0D, .gps_lat = -2, .gps_lon = 0x11223344};
    memcpy(c.pub, pub, sizeof(c.pub));
    c.out_path[0] = 0xAB;
    c.out_path[63] = 0xCD;
    memcpy(c.name, "Heltec", 6);
    memset(f, 0x55, MF_MAX_FRAME);
    CHECK(mf_encode_update_contact(f, &c) == 148);
    CHECK(f[0] == 9 && f[1] == pub[0] && f[33] == 1 && f[34] == 1 && f[35] == 0x41);
    CHECK(f[36] == 0xAB && f[99] == 0xCD && memcmp(f + 100, "Heltec", 6) == 0 && f[106] == 0);
    CHECK(f[132] == 0x04 && f[135] == 0x01);                     // last_advert_timestamp, little-endian
    CHECK(f[136] == 0xFE && f[139] == 0xFF);                     // gps_lat -2
    CHECK(f[140] == 0x44 && f[143] == 0x11);                     // gps_lon
    CHECK(f[144] == 0x0D && f[147] == 0x0A && f[148] == 0x55);   // lastmod, nothing written after it
}

static void limits(void) {
    uint8_t f[MF_MAX_FRAME];
    char    big[200];
    memset(big, 'a', sizeof(big));
    CHECK(mf_encode_send_channel(f, 0, 1, big, 161) == 0); // over MAX_TEXT_LEN
    CHECK(mf_encode_send_channel(f, 0, 1, big, 160) == 167);
    uint8_t const prefix[6] = {0};
    CHECK(mf_encode_send_dm(f, 0, 1, prefix, big, 161) == 0);
    CHECK(mf_encode_send_dm(f, 0, 1, prefix, big, 160) == 173 && 173 <= MF_MAX_FRAME);
    char name[300];
    memset(name, 'n', sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    CHECK(mf_encode_app_start(f, name) == 0);
}

static void text_cut(void) {
    CHECK(mf_text_cut("hello", 10) == 5);
    CHECK(mf_text_cut("hello", 3) == 3);
    CHECK(mf_text_cut("a\xC3\xA9" "b", 2) == 1); // would split the two-byte e-acute
    CHECK(mf_text_cut("a\xC3\xA9" "b", 3) == 3);
    CHECK(mf_text_cut("\xE2\x82\xAC", 2) == 0); // euro sign needs 3
    CHECK(mf_channel_text_max(8) == 150);
    CHECK(mf_channel_text_max(31) == 127);
    CHECK(mf_channel_text_max(200) == 0);
}

static void decoders(void) {
    mf_msg_t m;
    uint8_t  sent[10] = {6, 1, 0x11, 0x22, 0x33, 0x44, 0xE8, 0x03, 0, 0};
    mf_decode(sent, sizeof(sent), &m);
    CHECK(m.kind == MF_SENT && m.flood == 1 && m.ack == 0x44332211 && m.est_timeout == 1000);
    mf_decode(sent, 9, &m);
    CHECK(m.kind == MF_BAD);

    uint8_t err[2] = {1, 3};
    mf_decode(err, 2, &m);
    CHECK(m.kind == MF_ERR && m.err == 3);
    mf_decode(err, 1, &m);
    CHECK(m.kind == MF_BAD);

    uint8_t ok[1] = {0};
    mf_decode(ok, 1, &m);
    CHECK(m.kind == MF_OK);
    mf_decode(ok, 0, &m);
    CHECK(m.kind == MF_BAD);

    uint8_t conf[9] = {0x82, 1, 0, 0, 0, 0x10, 0, 0, 0};
    mf_decode(conf, 9, &m);
    CHECK(m.kind == MF_SEND_CONFIRMED && m.ack == 1 && m.trip_ms == 16);

    uint8_t info[58 + 3] = {5};
    memset(info + 4, 7, 32);
    memcpy(info + 58, "Bob", 3);
    mf_decode(info, sizeof(info), &m);
    CHECK(m.kind == MF_SELF_INFO && m.pub[0] == 7 && strcmp(m.name, "Bob") == 0);
    mf_decode(info, 20, &m);
    CHECK(m.kind == MF_BAD);

    uint8_t dev[2] = {13, 14};
    mf_decode(dev, 2, &m);
    CHECK(m.kind == MF_DEVICE_INFO && m.fw_ver == 14);

    uint8_t waiting[1] = {0x83}, none[1] = {10}, msg[16] = {17}, cmsg[16] = {16};
    mf_decode(waiting, 1, &m);
    CHECK(m.kind == MF_MSG_WAITING);
    mf_decode(none, 1, &m);
    CHECK(m.kind == MF_NO_MORE);
    mf_decode(msg, 16, &m);
    CHECK(m.kind == MF_MSG_RECV && m.is_channel);
    mf_decode(cmsg, 16, &m);
    CHECK(m.kind == MF_MSG_RECV && !m.is_channel);

    uint8_t unknown[4] = {0x8A, 1, 2, 3}; // NEW_ADVERT: not an error
    mf_decode(unknown, 4, &m);
    CHECK(m.kind == MF_UNKNOWN && m.code == 0x8A);
}

int main(void) {
    encoders();
    limits();
    text_cut();
    decoders();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_frame_test: all checks passed\n");
    return 0;
}
