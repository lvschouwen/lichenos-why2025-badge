// Host test: the message history journal on the SD card (#48): codec, scan, replay, writer, policies, compaction
#include "mesh_hist.h"
#include "mesh_outbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// ---- helpers -------------------------------------------------------------------------------------------------------

static uint8_t file[65536]; // a journal in memory
static size_t  file_len;

typedef struct {
    uint8_t const *p;
    size_t         len, pos, chunk; // chunk: the most one read gives, to cut records across reads
} mem_rd_t;

static size_t mem_read(void *ctx, uint8_t *buf, size_t cap) {
    mem_rd_t *r = (mem_rd_t *)ctx;
    size_t    n = r->len - r->pos;
    n           = n < cap ? n : cap;
    n           = r->chunk && n > r->chunk ? r->chunk : n;
    memcpy(buf, r->p + r->pos, n);
    r->pos += n;
    return n;
}

static uint8_t out_file[65536];
static size_t  out_len;
static int     fail_write_after = -1; // that many writes go through, then they fail

static bool mem_write(void *ctx, uint8_t const *buf, size_t len) {
    (void)ctx;
    if (fail_write_after == 0) {
        return false;
    }
    fail_write_after -= fail_write_after > 0;
    memcpy(out_file + out_len, buf, len);
    out_len += len;
    return true;
}

static void append(mesh_hist_rec_t const *r) {
    size_t n = mesh_hist_encode(r, file + file_len, sizeof(file) - file_len);
    CHECK(n > 0);
    file_len += n;
}

static mesh_hist_rec_t chat_rec(mesh_chat_key_t const *k, char const *title) {
    mesh_hist_rec_t r;
    memset(&r, 0, sizeof(r));
    r.type = MESH_HIST_CHAT;
    r.key  = *k;
    strcpy(r.title, title);
    return r;
}

static mesh_hist_rec_t msg_rec(mesh_chat_key_t const *k, uint32_t seq, char const *text) {
    mesh_hist_rec_t r;
    memset(&r, 0, sizeof(r));
    r.type     = MESH_HIST_MSG;
    r.seq      = seq;
    r.key      = *k;
    r.epoch    = 1700000000u + seq;
    r.rssi_x10 = -873;
    strcpy(r.text, text);
    return r;
}

#define MAX_RECS 256
static mesh_hist_rec_t got[MAX_RECS];
static size_t          got_n;

static bool collect(mesh_hist_rec_t const *r, size_t bytes, void *ctx) {
    (void)bytes, (void)ctx;
    if (got_n < MAX_RECS) {
        got[got_n++] = *r;
    }
    return true;
}

static mesh_hist_scan_t scan_bytes(uint8_t const *p, size_t len, size_t chunk) {
    static uint8_t   buf[MESH_HIST_SCAN_BUF];
    mem_rd_t         rd = {.p = p, .len = len, .chunk = chunk};
    mesh_hist_scan_t s;
    got_n = 0;
    mesh_hist_scan(mem_read, &rd, buf, collect, NULL, &s);
    return s;
}

static mesh_chat_key_t chan_key(char const *name) {
    mesh_chat_key_t k;
    mesh_chat_key_channel(&k, name);
    return k;
}

static mesh_chat_key_t dm_key(uint8_t b) {
    uint8_t         id[6] = {b, 2, 3, 4, 5, 6};
    mesh_chat_key_t k;
    mesh_chat_key_dm(&k, id);
    return k;
}

// ---- tests ---------------------------------------------------------------------------------------------------------

static void codec_checks(void) {
    mesh_chat_key_t ck = chan_key("#BadgeTest"), dk = dm_key(0xAB);
    uint8_t         buf[MESH_HIST_REC_MAX];
    mesh_hist_rec_t in, out;
    size_t          used = 0;

    in = chat_rec(&ck, "#BadgeTest");
    size_t n = mesh_hist_encode(&in, buf, sizeof(buf));
    CHECK(n == MESH_HIST_HDR + 2 + 10 + 1 + 10 && buf[0] == MESH_HIST_MAGIC && buf[1] == MESH_HIST_CHAT);
    CHECK(mesh_hist_decode(buf, n, &out, &used) == MESH_HIST_OK && used == n);
    CHECK(out.type == MESH_HIST_CHAT && memcmp(&out.key, &ck, sizeof(ck)) == 0 && strcmp(out.title, "#BadgeTest") == 0);

    // A DM message with the longest text and every field set
    in = msg_rec(&dk, 0xDEADBEEF, "");
    memset(in.text, 'x', MESH_TEXT_MAX);
    in.text[MESH_TEXT_MAX] = '\0';
    in.outgoing = 1, in.state = MESH_OUT_DELIVERED, in.repeats = 3, in.trip_ms = 123456;
    n = mesh_hist_encode(&in, buf, sizeof(buf));
    CHECK(n > 0 && n <= MESH_HIST_REC_MAX);
    CHECK(mesh_hist_decode(buf, n, &out, &used) == MESH_HIST_OK && used == n);
    CHECK(out.seq == 0xDEADBEEF && out.key.kind == MESH_CHAT_DM && out.key.id[0] == 0xAB && out.epoch == in.epoch &&
          out.rssi_x10 == -873 && out.outgoing == 1 && out.state == MESH_OUT_DELIVERED && out.repeats == 3 &&
          out.trip_ms == 123456 && strcmp(out.text, in.text) == 0);

    in.type     = MESH_HIST_READ;
    in.read_seq = 42;
    n           = mesh_hist_encode(&in, buf, sizeof(buf));
    CHECK(mesh_hist_decode(buf, n, &out, &used) == MESH_HIST_OK && out.type == MESH_HIST_READ && out.read_seq == 42);

    // Too small a buffer, a bad key or type: nothing encoded
    in = msg_rec(&ck, 1, "hello");
    CHECK(mesh_hist_encode(&in, buf, 12) == 0);
    in.key.kind = 9;
    CHECK(mesh_hist_encode(&in, buf, sizeof(buf)) == 0);
    in      = msg_rec(&ck, 1, "hello");
    in.type = 77;
    CHECK(mesh_hist_encode(&in, buf, sizeof(buf)) == 0);

    // Partial records want more; any changed byte is caught by the CRC; a wrong magic is bad at once
    in = msg_rec(&ck, 7, "hello");
    n  = mesh_hist_encode(&in, buf, sizeof(buf));
    CHECK(mesh_hist_decode(buf, 0, &out, &used) == MESH_HIST_MORE);
    CHECK(mesh_hist_decode(buf, 5, &out, &used) == MESH_HIST_MORE);
    CHECK(mesh_hist_decode(buf, n - 1, &out, &used) == MESH_HIST_MORE);
    for (size_t i = 1; i < n; i++) {
        buf[i] ^= 0x10;
        mesh_hist_dec_t d = mesh_hist_decode(buf, n, &out, &used);
        CHECK(d == MESH_HIST_BAD || (i == 2 || i == 3)); // a changed length can also read as "more"
        buf[i] ^= 0x10;
    }
    buf[0] = 0;
    CHECK(mesh_hist_decode(buf, n, &out, &used) == MESH_HIST_BAD);
}

// A record type from a newer firmware (good CRC) is skipped, not an error
static void unknown_type_checks(void) {
    mesh_chat_key_t ck = chan_key("public");
    mesh_hist_rec_t r  = msg_rec(&ck, 3, "hi");
    file_len           = 0;
    append(&r);
    size_t at = file_len;
    append(&r);
    file[at + 1] = 9; // retype the second record and fix its CRC by re-encoding the header the same way
    {
        // CRC-16/CCITT-FALSE over type..seq and the payload, as mesh_hist.c
        uint16_t crc  = 0xFFFF;
        size_t   plen = file[at + 2] | file[at + 3] << 8;
        for (size_t i = at + 1; i < at + MESH_HIST_HDR + plen; i++) {
            if (i == at + 8) {
                i = at + MESH_HIST_HDR; // skip the CRC field
            }
            crc ^= (uint16_t)(file[i] << 8);
            for (int b = 0; b < 8; b++) {
                crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
            }
        }
        file[at + 8] = (uint8_t)crc, file[at + 9] = (uint8_t)(crc >> 8);
    }
    append(&r);
    mesh_hist_scan_t s = scan_bytes(file, file_len, 0);
    CHECK(!s.torn && s.records == 3 && got_n == 3 && got[1].type == 9 && got[2].type == MESH_HIST_MSG);
}

static void scan_checks(void) {
    mesh_chat_key_t ck = chan_key("#badgetest");
    file_len           = 0;
    mesh_hist_rec_t r  = chat_rec(&ck, "#badgetest");
    append(&r);
    char text[40];
    for (uint32_t i = 1; i <= 100; i++) {
        snprintf(text, sizeof(text), "bob: message %u", (unsigned)i);
        r = msg_rec(&ck, i, text);
        append(&r);
    }
    // Whole, read in odd chunk sizes (records cut across reads) and in 4 KB ones
    size_t chunks[] = {0, 1, 7, 33, 4096};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        mesh_hist_scan_t s = scan_bytes(file, file_len, chunks[c]);
        CHECK(!s.torn && s.records == 101 && s.good_bytes == file_len && got_n == 101);
        CHECK(got[100].seq == 100 && strcmp(got[100].text, "bob: message 100") == 0);
    }
    // A torn tail: the last record cut short (power lost mid-write) fails, replay stops there
    mesh_hist_scan_t s = scan_bytes(file, file_len - 3, 7);
    CHECK(s.torn && s.records == 100 && got_n == 100 && got[99].seq == 99);
    // A torn record in the middle: nothing after it is trusted
    size_t mid = 0, used = 0;
    for (int i = 0; i < 51; i++) {
        mesh_hist_rec_t tmp;
        mesh_hist_decode(file + mid, file_len - mid, &tmp, &used);
        mid += used;
    }
    file[mid + MESH_HIST_HDR + 3] ^= 0xFF;
    s = scan_bytes(file, file_len, 0);
    CHECK(s.torn && s.records == 51 && s.good_bytes == mid);
    file[mid + MESH_HIST_HDR + 3] ^= 0xFF;
    // Garbage (zeros, as a FAT cluster that was allocated but never written) is torn too
    memset(file + file_len, 0, 32);
    s = scan_bytes(file, file_len + 32, 0);
    CHECK(s.torn && s.records == 101);
    // An empty journal is fine
    s = scan_bytes(file, 0, 0);
    CHECK(!s.torn && s.records == 0);
}

static void replay_checks(void) {
    static mesh_hist_t h;
    static uint8_t     pend[MESH_HIST_PENDING];
    mesh_store_set_hooks(NULL, NULL);
    CHECK(mesh_store_init(3));
    mesh_hist_init(&h, pend, sizeof(pend));
    mesh_chat_key_t ck = chan_key("#BadgeTest"), dk = dm_key(7);
    file_len           = 0;
    mesh_hist_rec_t r  = chat_rec(&ck, "#BadgeTest");
    append(&r);
    r = chat_rec(&dk, "@Bob");
    append(&r);
    r = msg_rec(&ck, 1, "a: one");
    append(&r);
    r = msg_rec(&dk, 2, "bob: hi");
    append(&r);
    r = msg_rec(&ck, 4, "c: four");
    append(&r);
    r          = msg_rec(&ck, 3, "me: three"); // an outgoing message journaled once final, after a later one
    r.outgoing = 1, r.state = MESH_OUT_ON_AIR;
    append(&r);
    r       = msg_rec(&ck, 3, "me: three"); // and again when a repeat was heard
    r.outgoing = 1, r.state = MESH_OUT_HEARD, r.repeats = 1;
    append(&r);
    r.type = MESH_HIST_READ, r.read_seq = 4;
    append(&r);
    mesh_hist_scan_t s = scan_bytes(file, file_len, 5);
    CHECK(!s.torn && got_n == 8);
    for (size_t i = 0; i < got_n; i++) {
        CHECK(mesh_hist_replay(&h, &got[i]));
    }
    CHECK(h.loaded == 5);
    mesh_msg_t m[4];
    CHECK(mesh_store_chat(&ck, m, 4, false) == 3);
    CHECK(m[0].seq == 1 && m[1].seq == 3 && m[2].seq == 4);
    CHECK(m[1].outgoing && m[1].state == MESH_OUT_HEARD && m[1].repeats == 1 && m[1].loaded && m[1].id == 0);
    CHECK(strcmp(m[0].channel, "#BadgeTest") == 0 && m[0].epoch == 1700000001u && m[0].rssi_x10 == -873);
    CHECK(mesh_store_chat(&dk, m, 4, false) == 1 && m[0].dm && m[0].peer[0] == 7 && strcmp(m[0].channel, "@Bob") == 0);
    CHECK(mesh_store_unread_total() == 0); // loaded messages count as read
    mesh_conv_t cv[4];
    size_t      n = mesh_store_convs(cv, 4);
    mesh_conv_sort(cv, n);
    CHECK(n == 2 && strcmp(cv[0].title, "#BadgeTest") == 0 && cv[0].last_seq == 4 && cv[0].last_loaded);
    // New messages continue after the loaded ones, and the journal already knows both chats
    CHECK(mesh_store_add_message("#BadgeTest", "d: live", -500, 9));
    CHECK(mesh_store_chat(&ck, m, 1, false) == 1 && m[0].seq == 5 && !m[0].loaded);
    CHECK(mesh_hist_add(&h, &ck, &m[0], 100));
    CHECK(h.pending_records == 1); // no second CHAT record
    // A message for a chat without a CHAT record still loads, titled by its key
    mesh_chat_key_t other = chan_key("#Lost");
    r                     = msg_rec(&other, 50, "x");
    CHECK(mesh_hist_replay(&h, &r));
    CHECK(mesh_store_chat(&other, m, 1, false) == 1 && strcmp(m[0].channel, "#lost") == 0);
}

static void decode_pending(mesh_hist_t const *h) {
    scan_bytes(h->buf, h->len, 0);
}

static void writer_checks(void) {
    static mesh_hist_t h;
    static uint8_t     pend[MESH_HIST_PENDING], fresh[MESH_HIST_PENDING];
    mesh_store_init(5);
    mesh_hist_init(&h, pend, sizeof(pend));
    mesh_chat_key_t ck = chan_key("#badgetest"), dk = dm_key(1);

    // Incoming: CHAT once, then MSG; the first record starts the 60 s clock
    mesh_msg_t m;
    memset(&m, 0, sizeof(m));
    strcpy(m.channel, "#badgetest");
    strcpy(m.text, "bob: hello");
    m.seq = 1, m.epoch = 99, m.rssi_x10 = -900;
    CHECK(mesh_hist_add(&h, &ck, &m, 500));
    m.seq = 2;
    CHECK(mesh_hist_add(&h, &ck, &m, 520));
    CHECK(h.oldest_s == 500 && h.pending_records == 3);
    decode_pending(&h);
    CHECK(got_n == 3 && got[0].type == MESH_HIST_CHAT && got[1].type == MESH_HIST_MSG && got[2].seq == 2);
    // A title change writes the CHAT again; an unknown contact name does not
    strcpy(m.channel, "#BadgeTest");
    m.seq = 3;
    mesh_hist_add(&h, &ck, &m, 530);
    CHECK(h.pending_records == 5);
    mesh_msg_t d;
    memset(&d, 0, sizeof(d));
    strcpy(d.channel, "@Bob");
    d.dm = 1, d.seq = 4;
    mesh_hist_add(&h, &dk, &d, 530);
    strcpy(d.channel, "@?");
    d.seq = 5;
    mesh_hist_add(&h, &dk, &d, 530);
    CHECK(h.pending_records == 8);

    // The swap hands the buffer out and carries on empty
    size_t   len;
    uint32_t recs;
    uint8_t *old = mesh_hist_swap(&h, fresh, &len, &recs);
    CHECK(old == pend && recs == 8 && len > 0 && h.len == 0 && h.buf == fresh && h.pending_records == 0);

    // Outgoing channel message: held while queued, written on air, again when heard, not again for the same state
    mesh_msg_t o;
    memset(&o, 0, sizeof(o));
    strcpy(o.channel, "#BadgeTest");
    strcpy(o.text, "me: test");
    o.outgoing = 1, o.id = 11, o.seq = 6, o.state = MESH_OUT_QUEUED;
    mesh_hist_out_new(&h, &ck, &o, 600);
    CHECK(h.len == 0 && mesh_hist_held_count(&h) == 1);
    mesh_hist_out_state(&h, 11, MESH_OUT_SENT, 0, 0, 0, 601);
    CHECK(h.len == 0);
    mesh_hist_out_state(&h, 11, MESH_OUT_ON_AIR, 0, 0, 0, 602);
    CHECK(h.pending_records == 1);
    mesh_hist_out_state(&h, 11, MESH_OUT_ON_AIR, 0, 0, 0, 603);
    CHECK(h.pending_records == 1);
    mesh_hist_out_state(&h, 11, MESH_OUT_HEARD, 1, 0, 0, 604);
    CHECK(h.pending_records == 2);
    decode_pending(&h);
    CHECK(got_n == 2 && got[0].seq == 6 && got[0].state == MESH_OUT_ON_AIR && got[1].state == MESH_OUT_HEARD &&
          got[1].repeats == 1 && got[1].outgoing);

    // Outgoing DM: on air is not final (waiting for the ACK), delivered is
    memset(&o, 0, sizeof(o));
    strcpy(o.channel, "@Bob");
    o.outgoing = 1, o.dm = 1, o.id = 12, o.seq = 7;
    mesh_hist_out_new(&h, &dk, &o, 610);
    mesh_hist_out_state(&h, 12, MESH_OUT_ON_AIR, 0, 1, 0, 611);
    CHECK(h.pending_records == 2);
    mesh_hist_out_state(&h, 12, MESH_OUT_DELIVERED, 0, 1, 840, 612);
    CHECK(h.pending_records == 3);

    // A DM still waiting when MeshCore stops is written as "not sent"
    o.id = 13, o.seq = 8, o.state = MESH_OUT_QUEUED;
    mesh_hist_out_new(&h, &dk, &o, 620);
    mesh_hist_out_state(&h, 13, MESH_OUT_ON_AIR, 0, 1, 0, 621);
    CHECK(h.pending_records == 3 && mesh_hist_held_count(&h) == 3);
    // The outbox still has 13 (and 11), not 12 any more: 12 leaves the table, already written, nothing new
    uint32_t ids[] = {11, 13};
    mesh_hist_out_sweep(&h, ids, 2, 625);
    CHECK(mesh_hist_held_count(&h) == 2 && h.pending_records == 3);
    mesh_hist_stop(&h, 630);
    CHECK(mesh_hist_held_count(&h) == 0 && h.pending_records == 4);
    decode_pending(&h);
    CHECK(got_n == 4 && got[3].seq == 8 && got[3].state == MESH_OUT_FAILED && got[3].outgoing);

    // A held message that leaves the outbox unfinished is written as "not sent" too
    o.id = 14, o.seq = 9, o.state = MESH_OUT_QUEUED;
    mesh_hist_out_new(&h, &dk, &o, 640);
    mesh_hist_out_sweep(&h, NULL, 0, 641);
    CHECK(mesh_hist_held_count(&h) == 0 && h.pending_records == 5);
    // The hold table full: the oldest is settled to make room
    for (uint32_t i = 0; i < MESH_HIST_HOLD + 1; i++) {
        o.id = 100 + i, o.seq = 100 + i, o.state = MESH_OUT_QUEUED;
        mesh_hist_out_new(&h, &dk, &o, 650);
    }
    CHECK(mesh_hist_held_count(&h) == MESH_HIST_HOLD && h.pending_records == 6);
    decode_pending(&h);
    CHECK(got[got_n - 1].seq == 100 && got[got_n - 1].state == MESH_OUT_FAILED);

    // A full buffer drops records and counts them
    mesh_hist_init(&h, pend, 100);
    m.seq = 1;
    strcpy(m.channel, "#badgetest");
    CHECK(mesh_hist_add(&h, &ck, &m, 1)); // CHAT + MSG, about 90 bytes
    size_t before = h.len;
    CHECK(!mesh_hist_add(&h, &ck, &m, 1));
    CHECK(h.dropped == 1 && h.len == before);
}

static void policy_checks(void) {
    struct {
        uint32_t age;
        size_t   fill;
        bool     stopping, flush;
    } const rows[] = {
        {0, 0, false, false},   {0, 0, true, false},     // nothing pending: never
        {59, 100, false, false}, {60, 100, false, true}, // the oldest record waits 60 s at most
        {0, 100, true, true},                            // stopping: always
        {0, MESH_HIST_PENDING / 4 * 3 - 1, false, false}, {0, MESH_HIST_PENDING / 4 * 3, false, true}, // 3/4 full
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        CHECK(mesh_hist_should_flush(rows[i].age, rows[i].fill, rows[i].stopping) == rows[i].flush);
    }
    // Compaction: torn always; over the cap; over 64 KB and twice the live size
    CHECK(mesh_hist_should_compact(100, 100, true));
    CHECK(!mesh_hist_should_compact(MESH_HIST_COMPACT_MIN, 1, false));
    CHECK(mesh_hist_should_compact(MESH_HIST_COMPACT_MIN + 1, 1000, false));
    CHECK(!mesh_hist_should_compact(200000, 100000, false));
    CHECK(mesh_hist_should_compact(200001, 100000, false));
    CHECK(mesh_hist_should_compact((uint64_t)MESH_HIST_CAP + 1, MESH_HIST_CAP, false));
    CHECK(!mesh_hist_should_compact(MESH_HIST_CAP, MESH_HIST_CAP, false));
    // Card space: room to grow to the cap plus the reserve
    CHECK(mesh_hist_space_ok((uint64_t)MESH_HIST_CAP + MESH_HIST_RESERVE, 0));
    CHECK(!mesh_hist_space_ok((uint64_t)MESH_HIST_CAP + MESH_HIST_RESERVE - 1, 0));
    CHECK(mesh_hist_space_ok(MESH_HIST_RESERVE + 1000, MESH_HIST_CAP - 1000));
    CHECK(mesh_hist_space_ok(MESH_HIST_RESERVE, (uint64_t)MESH_HIST_CAP + 5000)); // over the cap: compaction is next
    CHECK(!mesh_hist_space_ok(0, 0));
}

static mesh_hist_chat_t slots[16];

static void compact_checks(void) {
    mesh_chat_key_t a = chan_key("#a"), b = dm_key(2);
    file_len          = 0;
    mesh_hist_rec_t r = chat_rec(&a, "#a");
    append(&r);
    r = chat_rec(&b, "@?");
    append(&r);
    char text[16];
    uint32_t seq = 0;
    for (int i = 0; i < 6; i++) {
        snprintf(text, sizeof(text), "a%d", i);
        r = msg_rec(&a, ++seq, text);
        append(&r);
        if (i < 2) {
            snprintf(text, sizeof(text), "b%d", i);
            r = msg_rec(&b, ++seq, text);
            append(&r);
        }
    }
    r = chat_rec(&a, "#A"); // renamed late: the compacted CHAT has the last title
    append(&r);
    r.type = MESH_HIST_READ;
    append(&r);
    r = chat_rec(&b, "@Bob");
    append(&r);

    // Pass 1
    mesh_hist_table_t t;
    mesh_hist_table_init(&t, slots, 16);
    static uint8_t buf[MESH_HIST_SCAN_BUF];
    mem_rd_t       rd = {.p = file, .len = file_len, .chunk = 9};
    mesh_hist_scan_t s;
    {
        size_t          at = 0, used;
        mesh_hist_rec_t tmp;
        while (at < file_len && mesh_hist_decode(file + at, file_len - at, &tmp, &used) == MESH_HIST_OK) {
            mesh_hist_count(&t, &tmp, used);
            at += used;
        }
    }
    CHECK(t.n == 2 && !t.overflow);
    uint64_t live = mesh_hist_live_bytes(&t, 3);
    CHECK(live > 0 && live < file_len);

    // Pass 2, keeping 3 per chat, staged through a buffer that only fits two records
    uint8_t             stage[2 * MESH_HIST_REC_MAX];
    mesh_hist_compact_t st;
    out_len            = 0;
    fail_write_after   = -1;
    rd.pos             = 0;
    mesh_hist_compact(mem_read, &rd, buf, &t, 3, mem_write, NULL, stage, sizeof(stage), &st);
    CHECK(st.ok && st.bytes_out == out_len && st.records_out == 2 + 3 + 2);
    s = scan_bytes(out_file, out_len, 0);
    CHECK(!s.torn && got_n == 7);
    // Journal order; each chat's CHAT (with its last title) just before its first kept message; a0..a2 dropped
    CHECK(got[0].type == MESH_HIST_CHAT && got[0].key.kind == MESH_CHAT_DM && strcmp(got[0].title, "@Bob") == 0);
    CHECK(strcmp(got[1].text, "b0") == 0 && strcmp(got[2].text, "b1") == 0);
    CHECK(got[3].type == MESH_HIST_CHAT && strcmp(got[3].title, "#A") == 0);
    CHECK(strcmp(got[4].text, "a3") == 0 && got[4].seq == 6 && strcmp(got[6].text, "a5") == 0);
    // The result replays to the same newest messages, and compacting it again changes nothing
    uint64_t first = out_len;
    for (size_t i = 0; i < 16; i++) {
        slots[i].used = false;
    }
    mesh_hist_table_init(&t, slots, 16);
    for (size_t i = 0; i < got_n; i++) {
        mesh_hist_count(&t, &got[i], 1);
    }
    memcpy(file, out_file, out_len);
    file_len = out_len;
    rd       = (mem_rd_t){.p = file, .len = file_len};
    out_len  = 0;
    mesh_hist_compact(mem_read, &rd, buf, &t, 3, mem_write, NULL, stage, sizeof(stage), &st);
    CHECK(st.ok && out_len == first && memcmp(out_file, file, out_len) == 0);

    // A write that fails is reported
    rd.pos           = 0;
    out_len          = 0;
    fail_write_after = 0;
    mesh_hist_compact(mem_read, &rd, buf, &t, 3, mem_write, NULL, stage, sizeof(stage), &st);
    CHECK(!st.ok);
    fail_write_after = -1;

    // A table too small: the chats it cannot hold keep all their records
    mesh_hist_chat_t tiny[2];
    mesh_hist_table_init(&t, tiny, 2);
    for (size_t i = 0; i < got_n; i++) {
        mesh_hist_count(&t, &got[i], 1);
    }
    CHECK(t.n == 1 && t.overflow);
    rd.pos  = 0;
    out_len = 0;
    mesh_hist_compact(mem_read, &rd, buf, &t, 1, mem_write, NULL, stage, sizeof(stage), &st);
    CHECK(st.ok);
    s = scan_bytes(out_file, out_len, 0);
    CHECK(!s.torn && got_n == 2 + 4); // the tracked chat (b): CHAT + 1 message; the other: CHAT + all 3 of its messages
}

int main(void) {
    codec_checks();
    unknown_type_checks();
    scan_checks();
    replay_checks();
    writer_checks();
    policy_checks();
    compact_checks();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_hist_test: all checks passed\n");
    return 0;
}
