// Host test: MeshCore companion frame codec (#45)
#include "mesh_frame.h"
#include "mutate.h"

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

// Log in, status, logout and the CLI (#51): MyMesh's offsets for CMD_SEND_LOGIN, _STATUS_REQ, _LOGOUT, SEND_TXT_MSG
static void remote_encoders(void) {
    uint8_t f[MF_MAX_FRAME], pub[32], prefix[6] = {1, 2, 3, 4, 5, 6};
    memset(pub, 9, sizeof(pub));
    size_t n = mf_encode_login(f, pub, "hunter2");
    CHECK(n == 1 + 32 + 7 && f[0] == 26 && f[1] == 9 && f[32] == 9 && memcmp(f + 33, "hunter2", 7) == 0);
    CHECK(mf_encode_login(f, pub, "") == 33);
    CHECK(mf_encode_login(f, pub, "123456789012345") == 48);
    CHECK(mf_encode_login(f, pub, "1234567890123456") == 0); // MyMesh would cut it: refused instead
    CHECK(mf_encode_status_req(f, pub) == 33 && f[0] == 27 && f[32] == 9);
    CHECK(mf_encode_logout(f, pub) == 33 && f[0] == 29 && f[1] == 9);
    n = mf_encode_send_cli(f, 0, 0x01020304, prefix, "ver", 3);
    CHECK(n == 16 && f[0] == 2 && f[1] == 1 /* CLI_DATA */ && f[2] == 0 && f[3] == 4 && memcmp(f + 7, prefix, 6) == 0 &&
          memcmp(f + 13, "ver", 3) == 0);
}

static void remote_decoders(void) {
    mf_msg_t m;
    // login ok: new layout [0x85, admin, prefix6, tag4, acl, fw_level], legacy has only the prefix
    uint8_t ok[14] = {0x85, 1, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 3, 8};
    mf_decode(ok, sizeof(ok), &m);
    CHECK(m.kind == MF_LOGIN_SUCCESS && m.admin == 1 && m.acl == 3 && m.fw_level == 8 && m.rprefix[0] == 1 && m.rprefix[5] == 6);
    mf_decode(ok, 8, &m);
    CHECK(m.kind == MF_LOGIN_SUCCESS && m.admin == 1 && m.acl == 0 && m.fw_level == 0);
    mf_decode(ok, 7, &m);
    CHECK(m.kind == MF_BAD);
    uint8_t fail[8] = {0x86, 0, 6, 5, 4, 3, 2, 1};
    mf_decode(fail, 8, &m);
    CHECK(m.kind == MF_LOGIN_FAIL && m.rprefix[0] == 6 && m.rprefix[5] == 1);
    mf_decode(fail, 5, &m);
    CHECK(m.kind == MF_BAD);

    // status: RepeaterStats after [0x87, 0, prefix6]
    uint8_t st[8 + 56] = {0x87, 0, 1, 2, 3, 4, 5, 6};
    uint8_t *s = st + 8;
    s[0] = 0x10; s[1] = 0x10;           // battery 4112 mV
    s[2] = 3;                           // queue 3
    s[4] = 0x90; s[5] = 0xFF;           // noise floor -112
    s[6] = 0xA0; s[7] = 0xFF;           // last rssi -96
    s[8] = 100;                         // received
    s[12] = 50;                         // sent
    s[16] = 20;                         // air time
    s[20] = 0x10; s[21] = 0x0E;         // uptime 3600 s
    s[24] = 1; s[28] = 2; s[32] = 3; s[36] = 4;
    s[40] = 5;                          // err events
    s[42] = 0xF6; s[43] = 0xFF;         // last snr -10 (x4)
    s[44] = 6; s[46] = 7;
    s[48] = 30; s[52] = 9;
    mf_decode(st, sizeof(st), &m);
    CHECK(m.kind == MF_STATUS && m.status.bytes == 56 && m.status.batt_mv == 4112 && m.status.tx_queue == 3);
    CHECK(m.status.noise_floor == -112 && m.status.last_rssi == -96 && m.status.n_recv == 100 && m.status.n_sent == 50);
    CHECK(m.status.air_secs == 20 && m.status.up_secs == 3600 && m.status.sent_flood == 1 && m.status.recv_direct == 4);
    CHECK(m.status.err_events == 5 && m.status.last_snr_q == -10 && m.status.direct_dups == 6 && m.status.flood_dups == 7);
    CHECK(m.status.rx_air_secs == 30 && m.status.recv_errors == 9 && m.rprefix[2] == 3);
    mf_decode(st, 8 + 20, &m); // an older repeater: the first fields only, the rest 0
    CHECK(m.kind == MF_STATUS && m.status.bytes == 20 && m.status.n_recv == 100 && m.status.up_secs == 0 &&
          m.status.rx_air_secs == 0);
    mf_decode(st, 7, &m);
    CHECK(m.kind == MF_BAD);

    // a contact message from the offline queue: V3 [16, snr, 0, 0, prefix6, path_len, type, ts4, text]
    uint8_t cm[64] = {16, 20, 0, 0, 1, 2, 3, 4, 5, 6, 0xFF, 1, 9, 9, 9, 9};
    memcpy(cm + 16, "uptime ok", 9);
    mf_decode(cm, 16 + 9, &m);
    CHECK(m.kind == MF_MSG_RECV && !m.is_channel && m.msg_ok && m.txt_type == MF_TXT_CLI_DATA && strcmp(m.text, "uptime ok") == 0 &&
          m.rprefix[0] == 1);
    // the old layout [7, prefix6, path_len, type, ts4, text]
    uint8_t old[32] = {7, 1, 2, 3, 4, 5, 6, 0xFF, 0, 9, 9, 9, 9};
    memcpy(old + 13, "hi", 2);
    mf_decode(old, 15, &m);
    CHECK(m.msg_ok && m.txt_type == 0 && strcmp(m.text, "hi") == 0);
    // a room's post: signed text has the author's 4 key bytes before the text
    uint8_t post[40] = {16, 0, 0, 0, 1, 2, 3, 4, 5, 6, 0xFF, 2, 9, 9, 9, 9, 0xA1, 0xA2, 0xA3, 0xA4};
    memcpy(post + 20, "morning", 7);
    mf_decode(post, 27, &m);
    CHECK(m.msg_ok && m.txt_type == MF_TXT_SIGNED_PLAIN && m.author[0] == 0xA1 && m.author[3] == 0xA4 && strcmp(m.text, "morning") == 0);
    mf_decode(post, 18, &m); // signed but cut inside the author
    CHECK(m.kind == MF_BAD || !m.msg_ok);
    uint8_t big[MF_MAX_FRAME] = {16, 0, 0, 0, 1, 2, 3, 4, 5, 6, 0xFF, 1, 9, 9, 9, 9};
    memset(big + 16, 'x', MF_MAX_FRAME - 16);
    mf_decode(big, MF_MAX_FRAME, &m);
    CHECK(m.msg_ok && strlen(m.text) == MF_MAX_FRAME - 16);
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

// One valid frame of every code mf_decode reads (#212): the corpus the pin and the malformed run start from
typedef struct {
    uint8_t buf[MF_MAX_FRAME];
    size_t  len;
} frame_t;

static size_t corpus(frame_t *c) {
    size_t n = 0;
    // OK, ERR, SENT, NO_MORE, DEVICE_INFO, MSG_WAITING, PATH_UPDATED, SEND_CONFIRMED, LOGIN_FAIL, unknown
    static uint8_t const fixed[][10] = {
        {0}, {1, 3}, {6, 1, 0x11, 0x22, 0x33, 0x44, 0xE8, 3, 0, 0}, {10}, {13, 14}, {0x83}, {0x81},
        {0x82, 1, 2, 3, 4, 5, 6, 7, 8}, {0x86, 0, 6, 5, 4, 3, 2, 1}, {0x8A, 1, 2, 3},
    };
    static uint8_t const fixed_len[] = {1, 2, 10, 1, 2, 1, 1, 9, 8, 4};
    for (size_t i = 0; i < sizeof(fixed_len); i++, n++) {
        memcpy(c[n].buf, fixed[i], fixed_len[i]);
        c[n].len = fixed_len[i];
    }
    memset(c[n].buf, 0, sizeof(c[n].buf)); // SELF_INFO: [5, ..., pub at 4, name at 58]
    c[n].buf[0] = 5;
    memset(c[n].buf + 4, 7, MF_PUB_LEN);
    memcpy(c[n].buf + 58, "Alice", 5);
    c[n++].len = 63;
    uint8_t const msgs[][16] = {
        {7, 1, 2, 3, 4, 5, 6, 0xFF, 1, 9, 9, 9, 9, 'o', 'k', '!'},
        {16, 20, 0, 0, 1, 2, 3, 4, 5, 6, 0xFF, 2, 9, 9, 9, 9},
        {8, 0, 0xFF, 0, 1, 2, 3, 4, 'c', 'h', 'a', 'n'},
        {17, 0, 0, 0, 0, 0xFF, 0, 1, 2, 3, 4, 'v', '3'},
    };
    for (size_t i = 0; i < 4; i++, n++) {
        memcpy(c[n].buf, msgs[i], 16);
        memcpy(c[n].buf + 16, "AUTHtext of a message", 21);
        c[n].len = 37;
    }
    uint8_t const ok[14] = {0x85, 1, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 3, 8};
    memcpy(c[n].buf, ok, sizeof(ok));
    c[n++].len = sizeof(ok);
    c[n].buf[0] = 0x87; // STATUS with all 56 bytes of RepeaterStats
    for (size_t i = 1; i < 64; i++) {
        c[n].buf[i] = (uint8_t)(i * 7);
    }
    c[n++].len = 64;
    uint8_t const telem[] = {0x8B, 0, 1, 2, 3, 4, 5, 6, 1, 116, 0x01, 0x9C};
    memcpy(c[n].buf, telem, sizeof(telem));
    c[n++].len = sizeof(telem);
    uint8_t const trace[] = {0x89, 0, 3, 0, 0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 0xa1, 0x2a, 0xa1, 28, (uint8_t)-10, 4, 38};
    memcpy(c[n].buf, trace, sizeof(trace));
    c[n++].len = sizeof(trace);
    uint8_t const disc[] = {0x8D, 0, 0x2a, 0xe9, 0x6c, 0x65, 0x7d, 0x03, 0x41, 0xa1, 0xb2, 0x02, 0xc3, 0xd4};
    memcpy(c[n].buf, disc, sizeof(disc));
    c[n++].len = sizeof(disc);
    return n;
}

#define CORPUS_MAX 32

static size_t min_sz(size_t a, size_t b) {
    return a < b ? a : b;
}

// What any decoded frame must satisfy, valid or not: terminated strings, lengths that agree with the input, and a
// copied payload that is the input's bytes (ASan has the rest: the input is a heap block of exactly len bytes)
static bool decoded_sane(uint8_t const *f, size_t len, mf_msg_t const *m) {
    if (len == 0) {
        return m->kind == MF_BAD && m->code == 0;
    }
    bool ok = m->code == f[0] && m->kind <= MF_DISCOVERY;
    ok      = ok && memchr(m->name, 0, sizeof(m->name)) && memchr(m->text, 0, sizeof(m->text));
    ok      = ok && (!m->msg_ok || (m->kind == MF_MSG_RECV && !m->is_channel && strlen(m->text) <= len));
    ok      = ok && m->telem_len <= MF_TELEM_MAX && m->status.bytes <= MF_STATUS_BYTES;
    switch (m->kind) {
        case MF_SENT: return ok && len >= 10;
        case MF_SEND_CONFIRMED: return ok && len >= 9;
        case MF_SELF_INFO: {
            size_t nl = len >= 58 ? min_sz(len - 58, MF_NAME_MAX - 1) : 0;
            return ok && len >= 58 && memcmp(m->name, f + 58, nl) == 0 && m->name[nl] == 0;
        }
        case MF_LOGIN_SUCCESS:
        case MF_LOGIN_FAIL: return ok && len >= 8 && memcmp(m->rprefix, f + 2, MF_PREFIX) == 0;
        case MF_STATUS: return ok && len >= 8 && m->status.bytes == min_sz(len - 8, MF_STATUS_BYTES);
        case MF_TELEMETRY:
            return ok && len >= 8 && m->telem_len == min_sz(len - 8, MF_TELEM_MAX) &&
                   memcmp(m->telem, f + 8, m->telem_len) == 0;
        case MF_TRACE:
            return ok && m->trace.hash_len <= MF_TRACE_HASH_MAX && m->trace.hops <= MF_PATH_MAX &&
                   len == 13u + m->trace.hash_len + m->trace.hops && memcmp(m->trace.hashes, f + 12, m->trace.hash_len) == 0;
        case MF_DISCOVERY: return ok && len >= 10 && m->disc.out_len == f[8];
        default: return ok;
    }
}

static uint32_t fnv(uint32_t h, void const *p, size_t n) {
    uint8_t const *b = p;
    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}

// Malformed input (#212): every mutation of every corpus frame decodes to a sane result. The digest over all results
// pins the decoder's output byte for byte (mf_decode zeroes the struct first, so padding is 0); it was taken before the
// #212 split. A deliberate change to what mf_decode returns changes it: print the new value and say why in the commit.
static void decode_malformed(void) {
    frame_t  c[CORPUS_MAX];
    size_t   n      = corpus(c);
    uint32_t digest = 2166136261u;
    size_t   bad    = 0;
    for (size_t i = 0; i < n; i++) {
        mut_rng_t r;
        mut_seed(&r, (uint32_t)(i + 1) * 2654435761u);
        for (int k = 0; k < 3000; k++) {
            uint8_t buf[MF_MAX_FRAME + 64];
            size_t  len = k == 0 ? mut_truncate(c[i].buf, c[i].len, c[i].len, buf, sizeof(buf))
                                 : mut_random(&r, c[i].buf, c[i].len, buf, sizeof(buf));
            uint8_t *f  = mut_exact(buf, len);
            mf_msg_t m;
            mf_decode(f, len, &m);
            if (!decoded_sane(f, len, &m)) {
                if (bad++ < 5) {
                    printf("FAIL insane decode: corpus %zu mutation %d len %zu kind %d\n", i, k, len, (int)m.kind);
                }
            }
            digest = fnv(digest, &m, sizeof(m));
            free(f);
        }
    }
    failures += bad ? 1 : 0;
    uint32_t const pinned = 0x94a9306cu;
    if (digest != pinned) {
        printf("FAIL mf_decode output digest 0x%08x, pinned 0x%08x\n", (unsigned)digest, (unsigned)pinned);
        failures++;
    }
}

int main(void) {
    encoders();
    remote_encoders();
    remote_decoders();
    limits();
    text_cut();
    decoders();
    decode_malformed();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_frame_test: all checks passed\n");
    return 0;
}
