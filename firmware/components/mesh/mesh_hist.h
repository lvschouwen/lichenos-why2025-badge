#pragma once
// MeshCore's message history on the SD card (#48): an append-only journal of variable-length records that is replayed
// into mesh_store at boot. Pure logic, no ESP headers (host-tested in host_tests/mesh_hist_test.c); mesh_journal.c
// does the file work. Not thread-safe: the mesh task calls it under the mesh lock, except the codec, scan and
// compaction (they touch no shared state).
//
// Record: magic 0xA7, type, payload length (16 bit), seq (32 bit), CRC-16/CCITT over type..seq and the payload, then
// the payload. Little-endian. The framing is plaintext (scan, torn ends and compaction work on it without a key); the
// payload of every record but HEAD is encrypted (#98):
//   nonce (8 random bytes) | AES-128-CTR(plain payload) | tag (16 bytes)
// The counter block is seq | nonce | block index (big-endian); the tag is HMAC-SHA-256(type | seq | nonce | ciphertext)
// cut to 16 bytes (encrypt-then-MAC). The keys: root = SHA-256("lichen-journal-v1" | node private key), never stored;
// per journal enc = HMAC(root, "enc" | salt)[0..16), mac = HMAC(root, "mac" | salt), check = HMAC(root, "check" |
// salt)[0..16). A chat key in a plain payload is its kind, then the 6-byte prefix (DM) or the name's length and bytes
// (channel, lower-cased). Types:
//   HEAD  version 1, salt (16), check (16)   plaintext, the first record of every journal file
//   CHAT  key, title                         once per chat, again when its title changes
//   MSG   key, epoch, rssi, flags, state, repeats, trip_ms, text   seq = the store's seq of the message
//   READ  key, seq read up to                decoded, not written: loaded messages count as read

#include "mesh_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_HIST_MAGIC       0xA7
#define MESH_HIST_HDR         10
#define MESH_HIST_REC_MAX     256                    // header included; the longest MSG is 241 bytes encrypted
#define MESH_HIST_PENDING     16384                  // the pending buffer (PSRAM)
#define MESH_HIST_FLUSH_S     60                     // the oldest pending record waits at most this long
#define MESH_HIST_CAP         (16u * 1024u * 1024u)  // journal size that forces a compaction
#define MESH_HIST_RESERVE     (1u * 1024u * 1024u)   // card space kept free beyond the cap (log.tmp while compacting)
#define MESH_HIST_COMPACT_MIN (64u * 1024u)          // smaller journals are never compacted for size alone
#define MESH_HIST_HOLD        16                     // outgoing messages held until final (the outbox's OB_CAP)
#define MESH_HIST_KNOWN       256                    // chats whose CHAT record is known to be in the journal
#define MESH_HIST_CHUNK       4096                   // the scan reads this much at a time
#define MESH_HIST_SCAN_BUF    (MESH_HIST_CHUNK + MESH_HIST_REC_MAX)
#define MESH_HIST_RESYNC_MAX  (64u * 1024u)          // a bad spot longer than this ends the scan (torn)
#define MESH_HIST_DUP         8                      // outgoing seqs per chat a compaction remembers to spot rewrites

#define MESH_HIST_SALT        16
#define MESH_HIST_CHECK       16
#define MESH_HIST_NONCE       8
#define MESH_HIST_TAG         16
#define MESH_HIST_VERSION     1

typedef enum { MESH_HIST_CHAT = 1, MESH_HIST_MSG = 2, MESH_HIST_READ = 3, MESH_HIST_HEAD = 4 } mesh_hist_type_t;

// The journal's keys. root comes from the node identity; open once a HEAD's check matched (or a new journal began).
typedef struct {
    uint8_t root[32];
    bool    have_root, open;
    bool    error; // a primitive failed (hardware crypto, out of key slots): no verdict on any journal
    uint8_t salt[MESH_HIST_SALT];
    uint8_t enc[16], mac[32];
    void (*rand)(void *buf, size_t len); // nonces and salts
} mesh_hist_crypt_t;

// root = SHA-256("lichen-journal-v1" | prv)
bool mesh_hist_key(uint8_t root[32], uint8_t const *prv, size_t prv_len);
// A key holder for root (copied; the caller wipes its own copy), not open yet
void mesh_hist_crypt_init(mesh_hist_crypt_t *c, uint8_t const root[32], void (*rand)(void *buf, size_t len));
// A new journal: a random salt and its keys
bool mesh_hist_crypt_new(mesh_hist_crypt_t *c);
// A journal's HEAD: true (and open) when its check value is the one root gives for this salt
bool mesh_hist_crypt_open(mesh_hist_crypt_t *c, uint8_t const salt[MESH_HIST_SALT],
                          uint8_t const check[MESH_HIST_CHECK]);
// The HEAD record for c's salt; 0 when c is not open or cap too small
size_t mesh_hist_encode_head(mesh_hist_crypt_t const *c, uint8_t *out, size_t cap);

typedef struct {
    uint8_t         type;
    uint32_t        seq;
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 1]; // CHAT
    uint32_t        epoch;                       // MSG
    int16_t         rssi_x10;
    uint8_t         outgoing, state, repeats;
    uint32_t        trip_ms;
    char            text[MESH_TEXT_MAX + 1];
    uint32_t        read_seq; // READ
    uint8_t         salt[MESH_HIST_SALT], check[MESH_HIST_CHECK]; // HEAD
} mesh_hist_rec_t;

// The record (CHAT, MSG or READ) encrypted with c's keys, in out; 0 when c is not open, it does not fit in cap or the
// key is not valid
size_t mesh_hist_encode(mesh_hist_rec_t const *r, mesh_hist_crypt_t const *c, uint8_t *out, size_t cap);

typedef enum {
    MESH_HIST_OK,   // *r decoded (an unknown type is OK too, with only type and seq set), *used bytes consumed
    MESH_HIST_MORE, // in[] holds the start of a record only
    MESH_HIST_BAD,  // not a record: wrong magic, length or CRC, or a payload that does not parse (a torn write)
    MESH_HIST_AUTH, // a whole record (*used bytes) that c cannot read: no key open, a wrong tag, or no plaintext
} mesh_hist_dec_t;
// c may be NULL: then only HEAD and unknown types decode
mesh_hist_dec_t mesh_hist_decode(uint8_t const *in, size_t len, mesh_hist_crypt_t const *c, mesh_hist_rec_t *r,
                                 size_t *used);

// ---- reading a journal ---------------------------------------------------------------------------------------------

// Reads up to cap bytes; 0 at the end of the file (or on an error)
typedef size_t (*mesh_hist_read_fn)(void *ctx, uint8_t *buf, size_t cap);
// One good record (the HEAD too), its bytes as in the file; false stops the scan
typedef bool (*mesh_hist_rec_fn)(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx);

typedef struct {
    uint32_t records;
    uint64_t good_bytes;    // up to the end of the last good record
    uint32_t resyncs;       // bad spots skipped: the scan went on at the next record that decodes
    uint64_t skipped_bytes; // bytes in them
    bool     bad_seen;
    uint64_t first_bad;     // file offset of the first bad spot
    bool     torn;          // the file ends inside a record or in a bad spot, or a bad spot ran past RESYNC_MAX
    bool     stopped;       // the callback said stop
    bool     head;          // a HEAD opened the keys
    bool     unreadable;    // the first record is not a HEAD, or its check does not match this node's key: stopped
    uint32_t auth_failures; // records whose tag did not match: dropped
    bool     crypto_error;  // the HEAD could not be checked (a primitive failed): stopped, no verdict
} mesh_hist_scan_t;

// Decodes the journal from the start, record by record, to its end. A bad record (a torn write, a flipped bit) does
// not end it: the scan looks for the next magic byte whose record decodes (header, CRC and payload), up to
// MESH_HIST_RESYNC_MAX bytes on. buf holds MESH_HIST_SCAN_BUF bytes. c (with its root) is opened by the journal's
// HEAD; records that fail their tag are counted and skipped.
void mesh_hist_scan(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_crypt_t *c, mesh_hist_rec_fn on_rec,
                    void *ctx, mesh_hist_scan_t *out);

// ---- the writer's state (the mesh task, under the mesh lock) -------------------------------------------------------

typedef struct {
    bool            used, written;
    mesh_chat_key_t key;
    mesh_msg_t      msg;
    uint8_t         w_state, w_repeats; // what the journal has, once written
    uint32_t        w_trip_ms;
} mesh_hist_held_t;

typedef struct {
    bool            used;
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 1];
} mesh_hist_known_t;

typedef struct {
    mesh_hist_crypt_t const *crypt; // the open journal's keys
    uint8_t          *buf; // pending records, not yet in the file
    size_t            len, cap;
    uint32_t          pending_records;
    uint32_t          oldest_s; // when the first pending record came (the caller's clock)
    uint32_t          dropped;  // records that did not fit in the pending buffer
    uint32_t          loaded;   // MSG records replayed into the store
    mesh_hist_held_t  held[MESH_HIST_HOLD];
    mesh_hist_known_t known[MESH_HIST_KNOWN];
    size_t            known_next;
} mesh_hist_t;

// crypt: the keys of the journal the records go to (records are encrypted as they are added)
void mesh_hist_init(mesh_hist_t *h, uint8_t *buf, size_t cap, mesh_hist_crypt_t const *crypt);

// An incoming message (final at once): its chat's CHAT record if the journal lacks it, then the MSG. The title is the
// message's channel field ("#chan" or "@name"). false when the pending buffer had no room (counted in dropped).
bool mesh_hist_add(mesh_hist_t *h, mesh_chat_key_t const *key, mesh_msg_t const *m, uint32_t now_s);

// An outgoing message is journaled only once its state is final: a DM delivered or failed, a channel message on air,
// heard repeated or failed. A later change (more repeats, a retry that got through) is written again with the same seq,
// and the replay keeps the newest.
bool mesh_hist_out_final(mesh_msg_t const *m);
void mesh_hist_out_new(mesh_hist_t *h, mesh_chat_key_t const *key, mesh_msg_t const *m, uint32_t now_s);
void mesh_hist_out_state(mesh_hist_t *h, uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt,
                         uint32_t trip_ms, uint32_t now_s);
// The outbox holds only these ids now: a held message that left it is written as it stands (still pending: "not sent")
void mesh_hist_out_sweep(mesh_hist_t *h, uint32_t const *ids, size_t n, uint32_t now_s);
// MeshCore stops: every held message is written, the ones still pending as "not sent"
void mesh_hist_stop(mesh_hist_t *h, uint32_t now_s);
size_t mesh_hist_held_count(mesh_hist_t const *h);

// Flush the pending records now: always when stopping, else when the oldest has waited MESH_HIST_FLUSH_S or the
// buffer is 3/4 full. Never with nothing pending.
bool mesh_hist_should_flush(uint32_t oldest_age_s, size_t fill, bool stopping);
// After a compaction: the journal may have lost CHAT records of chats it dropped, so every chat gets one again
void mesh_hist_forget_chats(mesh_hist_t *h);
// Hands out the pending buffer (its length and record count) and carries on in fresh, empty
uint8_t *mesh_hist_swap(mesh_hist_t *h, uint8_t *fresh, size_t *len, uint32_t *records);

// Boot: one journal record into the store (CHAT titles are remembered, so the journal does not get them again).
// false when the store dropped a MSG.
bool mesh_hist_replay(mesh_hist_t *h, mesh_hist_rec_t const *r);

// ---- compaction ----------------------------------------------------------------------------------------------------

typedef struct {
    bool            used, written;
    mesh_chat_key_t key;
    char            title[MESH_CHANNEL_MAX + 1];
    uint32_t        msgs, seen; // messages (a rewritten outgoing one counts once), and pass 2's position
    uint64_t        msg_bytes;
    uint32_t        out_seq[MESH_HIST_DUP], out_index[MESH_HIST_DUP]; // its recent outgoing messages: rewrites
    bool            out_used[MESH_HIST_DUP];
    uint8_t         out_next;
} mesh_hist_chat_t;

typedef struct {
    mesh_hist_chat_t *slots;
    size_t            cap; // a power of two
    size_t            n;
    bool              overflow; // a chat did not fit: compaction keeps all of its records
} mesh_hist_table_t;

void mesh_hist_table_init(mesh_hist_table_t *t, mesh_hist_chat_t *slots, size_t cap);
// Pass 1: counts one record of the journal
void mesh_hist_count(mesh_hist_table_t *t, mesh_hist_rec_t const *r, size_t bytes);
// About what a compacted journal would hold: every chat's CHAT and its newest keep messages
uint64_t mesh_hist_live_bytes(mesh_hist_table_t const *t, size_t keep);
// A damaged journal (torn tail or bad spots) is always compacted: appending after a torn record would hide the appended
// ones behind a bad spot. Else when it is over MESH_HIST_CAP, or over MESH_HIST_COMPACT_MIN and twice the live size.
bool mesh_hist_should_compact(uint64_t file_bytes, uint64_t live_bytes, bool torn);
// The card can take the journal: room for it to grow to the cap, plus the reserve
bool mesh_hist_space_ok(uint64_t free_bytes, uint64_t log_bytes);

typedef bool (*mesh_hist_write_fn)(void *ctx, uint8_t const *buf, size_t len);
typedef struct {
    uint32_t records_out;
    uint64_t bytes_out;
    uint32_t auth_failures; // dropped
    bool     ok;            // every write went through, and the journal had its HEAD
} mesh_hist_compact_t;

// Pass 2 over the same journal (counted into t): writes its HEAD, each chat's CHAT (its last title) and the MSG records
// of its newest keep messages (every copy of a rewritten one: the replay keeps the last), in journal order; READ
// records go. The journal keeps its salt and keys: records are copied as they are (no new nonces), so records still
// pending in the writer, encrypted with these keys, can follow. buf as mesh_hist_scan's, out is a staging buffer of
// out_cap bytes (at least MESH_HIST_REC_MAX). c holds the root; the HEAD opens it.
void mesh_hist_compact(mesh_hist_read_fn read, void *rctx, uint8_t *buf, mesh_hist_crypt_t *c, mesh_hist_table_t *t,
                       size_t keep, mesh_hist_write_fn write, void *wctx, uint8_t *out, size_t out_cap,
                       mesh_hist_compact_t *st);

#ifdef __cplusplus
}
#endif
