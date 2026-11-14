// SPDX-FileCopyrightText: 2026 Lucas van Schouwen
// SPDX-License-Identifier: Apache-2.0
// lora_decode pinned before the #212 split (it is the P4 <-> C6 contract, #234, built into both images):
//  1. a table per message type, built with the real encoder: the valid frame, every truncation, one byte too long, bad
//     length fields, zero and maximum payloads, every version byte and every type byte. Each case's verdict and the
//     whole output struct (pre-filled, so untouched bytes count too) fold into one digest per group, recorded from the
//     unchanged decoder; the plain verdicts are also checked one by one.
//  2. a differential run of the live decoder against lora_decode_ref (the old function, verbatim) over random and
//     mutated valid frames, each in an exact-size heap buffer so an over-read trips ASan.
// Usage: lora_decode_pin_test [fuzz_cases] (default 200000; the one-off run in #212 used several million).
#include "lora_decode_ref.h"
#include "lora_proto.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define FILL 0xA5

static uint64_t fnv(uint64_t h, void const *data, size_t n) {
    uint8_t const *p = data;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

// One decode of buf[0..len) from an exact-size heap copy; returns the verdict, fills *out from FILL
static lora_dec_t decode_exact(
    lora_dec_t (*fn)(uint8_t const *, size_t, lora_msg_t *),
    uint8_t const *buf,
    size_t         len,
    lora_msg_t    *out,
    uint8_t        fill
) {
    uint8_t *copy = malloc(len ? len : 1);
    if (len) {
        memcpy(copy, buf, len);
    }
    memset(out, fill, sizeof(*out));
    lora_dec_t r = fn(len ? copy : copy + 1, len, out); // len 0: a pointer one past the end, any read traps
    free(copy);
    return r;
}

static size_t cases = 0;

// Decode with both decoders, require them to agree, fold the result into the group digest
static lora_dec_t pin(uint64_t *h, uint8_t const *buf, size_t len) {
    lora_msg_t live, ref;
    lora_dec_t r  = decode_exact(lora_decode, buf, len, &live, FILL);
    lora_dec_t r0 = decode_exact(lora_decode_ref, buf, len, &ref, FILL);
    CHECK(r == r0);
    CHECK(memcmp(&live, &ref, sizeof(live)) == 0);
    uint8_t v = (uint8_t)r;
    *h        = fnv(*h, &v, 1);
    *h        = fnv(*h, &live, sizeof(live));
    cases++;
    return r;
}

// The standard cases for one valid encoded frame
static void pin_frame(uint64_t *h, uint8_t const *frame, size_t n) {
    uint8_t buf[LORA_MSG_MAX + 2];
    CHECK(n >= 2 && n <= LORA_MSG_MAX);
    CHECK(pin(h, frame, n) == LORA_DEC_OK);
    for (size_t l = 0; l < n; l++) { // every truncation
        CHECK(pin(h, frame, l) == LORA_DEC_BAD_LEN);
    }
    memcpy(buf, frame, n);
    buf[n] = 0x5A; // one byte too long
    CHECK(pin(h, buf, n + 1) == LORA_DEC_BAD_LEN);
    buf[n] = 0x00;
    CHECK(pin(h, buf, n + 1) == LORA_DEC_BAD_LEN);
    for (int v = 0; v < 256; v++) { // every version byte
        buf[0]       = (uint8_t)v;
        lora_dec_t r = pin(h, buf, n);
        CHECK(v == LORA_PROTO_VERSION ? r == LORA_DEC_OK : r == LORA_DEC_BAD_VERSION);
    }
    buf[0] = LORA_PROTO_VERSION;
    for (int t = 0; t < 256; t++) { // every type byte on this body: unknown, or another type's length rules
        buf[1] = (uint8_t)t;
        pin(h, buf, n);
    }
}

// A length byte at off that the decoder checks against what follows: every value of it
static void pin_len_byte(uint64_t *h, uint8_t const *frame, size_t n, size_t off) {
    uint8_t buf[LORA_MSG_MAX];
    memcpy(buf, frame, n);
    for (int v = 0; v < 256; v++) {
        buf[off]     = (uint8_t)v;
        lora_dec_t r = pin(h, buf, n);
        CHECK(v == frame[off] ? r == LORA_DEC_OK : r == LORA_DEC_BAD_LEN);
    }
}

static size_t enc(lora_msg_t const *m, uint8_t *buf) {
    size_t n = lora_encode(m, buf, LORA_MSG_MAX);
    CHECK(n > 0);
    return n;
}

static void fill_pattern(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)(seed + i * 37);
    }
}

typedef struct {
    char const *name;
    uint64_t    digest; // recorded from the decoder at c5ef5200
} group_t;

static uint64_t group_config(void) {
    uint64_t   h = 0xcbf29ce484222325ull;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t m = {.type = LORA_MSG_CONFIG, .u.config = {0x89ABCDEF, 0x01234567, 12, 8, 0x34, 0xBEEF, -9}};
    size_t     n = enc(&m, buf);
    pin_frame(&h, buf, n);
    lora_msg_t out;
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.config.freq_hz == 0x89ABCDEF &&
        out.u.config.bw_hz == 0x01234567 && out.u.config.sf == 12 && out.u.config.cr == 8 &&
        out.u.config.sync_word == 0x34 && out.u.config.preamble == 0xBEEF && out.u.config.tx_power_dbm == -9
    );
    return h;
}

static uint64_t group_bodyless(void) {
    uint64_t      h       = 0xcbf29ce484222325ull;
    uint8_t const types[] = {LORA_MSG_RX_START, LORA_MSG_RX_STOP, LORA_MSG_STATUS_REQ, LORA_MSG_IDENT_REQ};
    for (size_t i = 0; i < sizeof(types); i++) {
        uint8_t    buf[LORA_MSG_MAX];
        lora_msg_t m = {.type = types[i]};
        CHECK(enc(&m, buf) == 2);
        pin_frame(&h, buf, 2);
    }
    return h;
}

static uint64_t group_send(void) {
    uint64_t h      = 0xcbf29ce484222325ull;
    int      lens[] = {1, 2, 100, 254, 255};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint8_t    buf[LORA_MSG_MAX];
        lora_msg_t m = {.type = LORA_MSG_SEND};
        m.u.tx.len   = (uint8_t)lens[i];
        fill_pattern(m.u.tx.data, m.u.tx.len, (uint8_t)i);
        size_t n = enc(&m, buf);
        pin_frame(&h, buf, n);
        pin_len_byte(&h, buf, n, 2);
        lora_msg_t out;
        CHECK(
            lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.tx.len == lens[i] &&
            memcmp(out.u.tx.data, m.u.tx.data, m.u.tx.len) == 0
        );
    }
    uint8_t zero[] = {LORA_PROTO_VERSION, LORA_MSG_SEND, 0}; // zero-length payload, never sent
    CHECK(pin(&h, zero, sizeof(zero)) == LORA_DEC_BAD_LEN);
    return h;
}

static uint64_t group_rx(void) {
    uint64_t h      = 0xcbf29ce484222325ull;
    int      lens[] = {1, 2, 64, 254, 255};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint8_t    buf[LORA_MSG_MAX];
        lora_msg_t m    = {.type = LORA_MSG_RX_PACKET};
        m.u.rx.rssi_x10 = (int16_t)(-1234 + (int)i);
        m.u.rx.snr_x10  = (int16_t)(-56 - (int)i);
        m.u.rx.c6_ms    = 0xFEDCBA98u + (uint32_t)i;
        m.u.rx.len      = (uint8_t)lens[i];
        fill_pattern(m.u.rx.data, m.u.rx.len, (uint8_t)(i + 9));
        size_t n = enc(&m, buf);
        pin_frame(&h, buf, n);
        pin_len_byte(&h, buf, n, 10);
        lora_msg_t out;
        CHECK(
            lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.rx.rssi_x10 == m.u.rx.rssi_x10 &&
            out.u.rx.snr_x10 == m.u.rx.snr_x10 && out.u.rx.c6_ms == m.u.rx.c6_ms && out.u.rx.len == lens[i] &&
            memcmp(out.u.rx.data, m.u.rx.data, m.u.rx.len) == 0
        );
    }
    uint8_t zero[11] = {LORA_PROTO_VERSION, LORA_MSG_RX_PACKET}; // zero-length payload
    CHECK(pin(&h, zero, sizeof(zero)) == LORA_DEC_BAD_LEN);
    return h;
}

static uint64_t group_status(void) {
    uint64_t      h = 0xcbf29ce484222325ull;
    uint8_t       buf[LORA_MSG_MAX];
    lora_status_t s = {LORA_STATE_TX, -987, -705, 1, 2, 3, 4, 5, 0x80000006u, 0xFFFFFFF7u, 8, 0xFFFE};
    lora_msg_t    m = {.type = LORA_MSG_STATUS, .u.status = s};
    size_t        n = enc(&m, buf);
    pin_frame(&h, buf, n);
    lora_msg_t out;
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.status.state == LORA_STATE_TX &&
        out.u.status.rssi_x10 == -987 && out.u.status.last_radio_err == -705 && out.u.status.packets == 1 &&
        out.u.status.crc_errors == 2 && out.u.status.header_errors == 3 && out.u.status.dropped == 4 &&
        out.u.status.ignored == 5 && out.u.status.uptime_s == 0x80000006u && out.u.status.heap_free == 0xFFFFFFF7u &&
        out.u.status.heap_min == 8 && out.u.status.stack_free == 0xFFFE
    );
    return h;
}

static uint64_t group_result(void) {
    uint64_t   h = 0xcbf29ce484222325ull;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t m = {.type = LORA_MSG_RESULT, .u.result = {LORA_MSG_SWEEP_REQ, LORA_RESULT_BAD_STATE, -32768}};
    size_t     n = enc(&m, buf);
    pin_frame(&h, buf, n);
    lora_msg_t out;
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.result.for_type == LORA_MSG_SWEEP_REQ &&
        out.u.result.code == LORA_RESULT_BAD_STATE && out.u.result.radio_err == -32768
    );
    return h;
}

static uint64_t group_ident(void) {
    uint64_t   h = 0xcbf29ce484222325ull;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t m = {.type = LORA_MSG_IDENT};
    fill_pattern(m.u.ident.app_elf_sha256, LORA_IDENT_SHA_LEN, 0x42);
    size_t n = enc(&m, buf);
    pin_frame(&h, buf, n);
    lora_msg_t out;
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK &&
        memcmp(out.u.ident.app_elf_sha256, m.u.ident.app_elf_sha256, LORA_IDENT_SHA_LEN) == 0
    );
    return h;
}

static uint64_t group_sweep_req(void) {
    uint64_t   h = 0xcbf29ce484222325ull;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t m = {.type = LORA_MSG_SWEEP_REQ, .u.sweep_req = {0x33221100, 0x77665544, 0, 255, 0xC3}};
    size_t     n = enc(&m, buf); // the decoder takes any count and samples; lora_sweep_req_valid judges them
    pin_frame(&h, buf, n);
    lora_msg_t out;
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.sweep_req.start_hz == 0x33221100 &&
        out.u.sweep_req.step_hz == 0x77665544 && out.u.sweep_req.count == 0 && out.u.sweep_req.samples == 255 &&
        out.u.sweep_req.seq == 0xC3
    );
    return h;
}

static uint64_t group_sweep(void) {
    uint64_t h        = 0xcbf29ce484222325ull;
    int      counts[] = {1, 2, 127, LORA_SWEEP_MAX_BINS};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint8_t    buf[LORA_MSG_MAX];
        lora_msg_t m               = {.type = LORA_MSG_SWEEP};
        m.u.sweep.start_hz         = 868000000u + (uint32_t)i;
        m.u.sweep.step_hz          = 25000u + (uint32_t)i;
        m.u.sweep.seq              = (uint8_t)(0xF0 + i);
        m.u.sweep.channel_rssi_x10 = (int16_t)(-1100 - (int)i);
        m.u.sweep.count            = (uint8_t)counts[i];
        for (int j = 0; j < counts[i]; j++) {
            m.u.sweep.rssi_x10[j] = (int16_t)(-1300 + j * 9 - (int)i);
        }
        size_t n = enc(&m, buf);
        pin_frame(&h, buf, n);
        pin_len_byte(&h, buf, n, 13); // count 0, over the maximum, or not matching what follows
        lora_msg_t out;
        CHECK(
            lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.sweep.start_hz == m.u.sweep.start_hz &&
            out.u.sweep.step_hz == m.u.sweep.step_hz && out.u.sweep.seq == m.u.sweep.seq &&
            out.u.sweep.channel_rssi_x10 == m.u.sweep.channel_rssi_x10 && out.u.sweep.count == counts[i] &&
            memcmp(out.u.sweep.rssi_x10, m.u.sweep.rssi_x10, 2u * (size_t)counts[i]) == 0
        );
    }
    // 129 bins with the bytes to match: still over the maximum (and over LORA_MSG_MAX's struct room)
    uint8_t big[14 + 2 * 129] = {LORA_PROTO_VERSION, LORA_MSG_SWEEP};
    big[13]                   = 129;
    CHECK(pin(&h, big, sizeof(big)) == LORA_DEC_BAD_LEN);
    uint8_t zero[14] = {LORA_PROTO_VERSION, LORA_MSG_SWEEP}; // count 0
    CHECK(pin(&h, zero, sizeof(zero)) == LORA_DEC_BAD_LEN);
    return h;
}

static uint64_t group_reg(void) {
    uint64_t   h = 0xcbf29ce484222325ull;
    uint8_t    buf[LORA_MSG_MAX];
    lora_msg_t out;
    lora_msg_t get = {.type = LORA_MSG_REG_GET, .u.reg_get = {0xA1B2, 0x7E}};
    size_t     n   = enc(&get, buf);
    pin_frame(&h, buf, n);
    CHECK(lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.reg_get.id == 0xA1B2 && out.u.reg_get.seq == 0x7E);
    lora_msg_t set = {.type = LORA_MSG_REG_SET, .u.reg_set = {0x0102, 0xF0E0D0C0u, 0x99}};
    n              = enc(&set, buf);
    pin_frame(&h, buf, n);
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.reg_set.id == 0x0102 && out.u.reg_set.value == 0xF0E0D0C0u &&
        out.u.reg_set.seq == 0x99
    );
    lora_msg_t val = {.type = LORA_MSG_REG_VAL, .u.reg_val = {0xFFFF, 0x01, 0xFE, 0x7F, 0x80000001u}};
    n              = enc(&val, buf);
    pin_frame(&h, buf, n);
    CHECK(
        lora_decode(buf, n, &out) == LORA_DEC_OK && out.u.reg_val.id == 0xFFFF && out.u.reg_val.seq == 0x01 &&
        out.u.reg_val.status == 0xFE && out.u.reg_val.type == 0x7F && out.u.reg_val.value == 0x80000001u
    );
    return h;
}

// Short or odd frames outside any one type: lengths 0-300 of all-zero, all-0xFF and version+type headers
static uint64_t group_misc(void) {
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t  buf[LORA_MSG_MAX + 1];
    for (int fill = 0; fill < 3; fill++) {
        memset(buf, fill == 1 ? 0xFF : 0x00, sizeof(buf));
        if (fill == 2) {
            buf[0] = LORA_PROTO_VERSION;
        }
        for (size_t l = 0; l <= sizeof(buf); l++) {
            pin(&h, buf, l);
        }
    }
    return h;
}

static void pinned_table(int record) {
    struct {
        group_t g;
        uint64_t (*run)(void);
    } groups[] = {
        {{"config", 0x358b4e1c395216a0ull}, group_config},
        {{"bodyless", 0x039b058b5e85caf1ull}, group_bodyless},
        {{"send", 0x82d1c7c72834708bull}, group_send},
        {{"rx", 0xb6072bb231d4c62eull}, group_rx},
        {{"status", 0x850d3ead566c01a4ull}, group_status},
        {{"result", 0x8e8949a80377a517ull}, group_result},
        {{"ident", 0xa67a194b061a4ef8ull}, group_ident},
        {{"sweep_req", 0xe8d92dcf5aa1ec5aull}, group_sweep_req},
        {{"sweep", 0xbc833bb8ebae88abull}, group_sweep},
        {{"reg", 0xe3fa52b40d2785caull}, group_reg},
        {{"misc", 0x68c08533bd19d31full}, group_misc},
    };
    for (size_t i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        uint64_t d = groups[i].run();
        if (record) {
            printf("%s 0x%016llxull\n", groups[i].g.name, (unsigned long long)d);
        } else if (d != groups[i].g.digest) {
            printf(
                "FAIL group %s: digest 0x%016llx, pinned 0x%016llx\n",
                groups[i].g.name,
                (unsigned long long)d,
                (unsigned long long)groups[i].g.digest
            );
            failures++;
        }
    }
}

// --- differential fuzz ---

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

static uint8_t const known_types[] = {
    LORA_MSG_CONFIG,
    LORA_MSG_RX_START,
    LORA_MSG_RX_STOP,
    LORA_MSG_STATUS_REQ,
    LORA_MSG_SEND,
    LORA_MSG_IDENT_REQ,
    LORA_MSG_SWEEP_REQ,
    LORA_MSG_REG_GET,
    LORA_MSG_REG_SET,
    LORA_MSG_RX_PACKET,
    LORA_MSG_STATUS,
    LORA_MSG_RESULT,
    LORA_MSG_IDENT,
    LORA_MSG_SWEEP,
    LORA_MSG_REG_VAL,
};

// A valid frame of a random type with random fields, from the encoder
static size_t random_valid(uint8_t *buf) {
    lora_msg_t m;
    for (size_t i = 0; i < sizeof(m); i++) {
        ((uint8_t *)&m)[i] = (uint8_t)rnd();
    }
    m.type = known_types[rnd() % sizeof(known_types)];
    if (m.type == LORA_MSG_SEND && m.u.tx.len == 0) {
        m.u.tx.len = 1;
    }
    if (m.type == LORA_MSG_RX_PACKET && m.u.rx.len == 0) {
        m.u.rx.len = 1;
    }
    if (m.type == LORA_MSG_SWEEP) {
        m.u.sweep.count = (uint8_t)(1 + rnd() % LORA_SWEEP_MAX_BINS);
    }
    size_t n = lora_encode(&m, buf, LORA_MSG_MAX);
    CHECK(n > 0);
    return n;
}

// Random or mutated-valid input into buf; returns its length (0..LORA_MSG_MAX)
static size_t fuzz_input(uint8_t *buf) {
    size_t n;
    if (rnd() % 4 == 0) { // fully random, with a plausible header most of the time
        n = rnd() % (LORA_MSG_MAX + 1);
        for (size_t i = 0; i < n; i++) {
            buf[i] = (uint8_t)rnd();
        }
        if (n > 0 && rnd() % 8) {
            buf[0] = LORA_PROTO_VERSION;
        }
        if (n > 1 && rnd() % 4) {
            buf[1] = known_types[rnd() % sizeof(known_types)];
        }
        return n;
    }
    n        = random_valid(buf);
    int muts = (int)(rnd() % 4);
    for (int k = 0; k < muts; k++) {
        switch (rnd() % 5) {
            case 0: n = rnd() % (n + 1); break; // truncate
            case 1:                             // extend
                while (n < LORA_MSG_MAX && rnd() % 2) {
                    buf[n++] = (uint8_t)rnd();
                }
                break;
            case 2:
                if (n) {
                    buf[rnd() % n] = (uint8_t)rnd();
                }
                break;
            case 3:
                if (n > 1) {
                    buf[1] = rnd() % 2 ? known_types[rnd() % sizeof(known_types)] : (uint8_t)rnd();
                }
                break;
            default: // a length byte off by a little
                if (n > 2) {
                    size_t at = buf[1] == LORA_MSG_RX_PACKET ? 10 : buf[1] == LORA_MSG_SWEEP ? 13 : 2;
                    if (at < n) {
                        buf[at] = (uint8_t)(buf[at] + (int)(rnd() % 5) - 2);
                    }
                }
                break;
        }
    }
    return n;
}

static void differential(long count) {
    long diffs = 0, ok = 0;
    for (long i = 0; i < count; i++) {
        uint8_t    buf[LORA_MSG_MAX];
        size_t     n    = fuzz_input(buf);
        uint8_t    fill = (uint8_t)rnd();
        lora_msg_t live, ref;
        lora_dec_t r  = decode_exact(lora_decode, buf, n, &live, fill);
        lora_dec_t r0 = decode_exact(lora_decode_ref, buf, n, &ref, fill);
        if (r != r0 || memcmp(&live, &ref, sizeof(live)) != 0) {
            if (diffs++ < 5) {
                printf(
                    "FAIL differential case %ld: len %zu type 0x%02x verdict %d vs ref %d\n",
                    i,
                    n,
                    n > 1 ? buf[1] : 0,
                    r,
                    r0
                );
            }
        }
        ok += r == LORA_DEC_OK;
    }
    printf("differential: %ld cases, %ld decoded OK, %ld differences\n", count, ok, diffs);
    failures += diffs > 0;
}

int main(int argc, char **argv) {
    int  record = argc > 1 && strcmp(argv[1], "--record") == 0;
    long fuzz   = argc > 1 && !record ? strtol(argv[1], NULL, 10) : 200000;
    pinned_table(record);
    printf("pinned: %zu cases\n", cases);
    if (!record) {
        differential(fuzz);
    }
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("lora_decode_pin_test: all passed\n");
    return 0;
}
