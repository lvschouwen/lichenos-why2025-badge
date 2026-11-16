#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The contact list on the SD card (#472). MeshCore's table holds up to MESH_CTAB_CAP_CARD contacts while this badge's
// card file is in use and MESH_CTAB_CAP_INTERNAL otherwise. The card file holds the whole list, encrypted like the
// message journal; MeshCore's own "contacts3" on /storage keeps a core set, so the badge is whole without the card.
// Everything here works on contacts3's 152-byte records, as DataStore writes them. Pure logic, host-tested
// (host_tests/mesh_ctab_test.c); mesh_ctab_run.cpp does the files. Not thread safe.

#define MESH_CTAB_REC          152
#define MESH_CTAB_PUB          32
#define MESH_CTAB_AT_NAME      32
#define MESH_CTAB_NAME         32
#define MESH_CTAB_AT_TYPE      64
#define MESH_CTAB_AT_FLAGS     65
#define MESH_CTAB_AT_ADVERT    72 // last_advert_timestamp
#define MESH_CTAB_AT_LASTMOD   140
#define MESH_CTAB_FAV          0x01
#define MESH_CTAB_CAP_CARD     4000 // owner, #472
#define MESH_CTAB_CAP_INTERNAL 1000 // the cap before #472, and what /storage is asked to hold at most
#define MESH_CTAB_CORE         250  // on /storage while the card holds the list

uint32_t mesh_ctab_lastmod(uint8_t const *rec);

// ---- which contacts /storage keeps ----------------------------------------------------------------------------------

#define MESH_CTAB_OUT    0
#define MESH_CTAB_ALWAYS 1 // a favourite, or one always() names (a conversation, a saved login)
#define MESH_CTAB_RECENT 2 // fills the set up to its limit, the most recently changed first

// Marks keep[i] for each of the n records: every always-in one whatever the limit, then the newest others (by lastmod,
// the earlier record on a tie) while fewer than limit are kept. order: n entries of scratch. Returns how many are kept.
size_t mesh_ctab_select(uint8_t const *recs, size_t n, size_t limit, bool (*always)(uint8_t const *rec, void *ctx),
                        void *ctx, uint8_t *keep, uint16_t *order);
// Moves the kept records to the front, in their order; how many
size_t mesh_ctab_compact(uint8_t *recs, size_t n, uint8_t const *keep);
// A number that changes when the always-in group does (a member more or less, a name, a type, the favourite flag):
// the owner changed something, so /storage is written at once and not at its interval
uint32_t mesh_ctab_digest(uint8_t const *recs, size_t n, uint8_t const *keep);

// ---- merging the card's records into the table ----------------------------------------------------------------------

typedef enum { MESH_CTAB_SKIP, MESH_CTAB_ADD, MESH_CTAB_REPLACE } mesh_ctab_merge_t;
// One record of the card: a contact the owner deleted stays deleted; one the table lacks is added; one it has is
// replaced only by a newer lastmod
mesh_ctab_merge_t mesh_ctab_merge_plan(bool deleted, bool in_table, uint32_t table_lastmod, uint32_t card_lastmod);

// ---- timestamp-only saves stay parked (#429) ------------------------------------------------------------------------

#define MESH_CTAB_SAVE_INTERVAL_S (15 * 60) // owner, #429: a save that only moved timestamps is written this often

typedef enum {
    MESH_CTAB_SAME,       // every byte equal
    MESH_CTAB_TIMES_ONLY, // only last_advert_timestamp and/or lastmod differ
    MESH_CTAB_CHANGED,    // anything else
} mesh_ctab_diff_t;

// Compares n records of a (the file) with n of b (the save); the worst difference found
mesh_ctab_diff_t mesh_ctab_diff(uint8_t const *a, uint8_t const *b, size_t n);
// Whether a save with this difference stays parked: only timestamps moved, nothing forces a write (a stop, the owner,
// the interval), and the last write is less than the interval ago
bool mesh_ctab_defer(mesh_ctab_diff_t d, bool forced, uint32_t now_s, uint32_t last_write_s);
// Whether a parked save is due to be written
bool mesh_ctab_flush_due(uint32_t now_s, uint32_t last_write_s);

// ---- contacts the owner deleted (kept on /storage until both files no longer hold them) ------------------------------

#define MESH_CTAB_DEL      64
#define MESH_CTAB_DEL_KEY  8  // the key's first bytes, as the advert table names a node
#define MESH_CTAB_DEL_WAIT 60 // seconds a deletion may take to reach the table before "in the table" means "back"
#define MESH_CTAB_DEL_FILE (12 + MESH_CTAB_DEL * MESH_CTAB_DEL_KEY)

typedef struct {
    uint8_t  key[MESH_CTAB_DEL][MESH_CTAB_DEL_KEY]; // oldest first
    uint32_t added_s[MESH_CTAB_DEL];
    bool     settled[MESH_CTAB_DEL]; // the table no longer had it at the last review
    size_t   n;
    uint32_t changes; // one more whenever the keys change: the file is rewritten
} mesh_ctab_del_t;

// Remembers a deleted contact; a full list forgets its oldest. A key already there only gets the new time.
void mesh_ctab_del_add(mesh_ctab_del_t *d, uint8_t const *pub, uint32_t now_s);
bool mesh_ctab_del_has(mesh_ctab_del_t const *d, uint8_t const *pub);
// When the table is copied for a save: an entry whose contact is in the table again after MESH_CTAB_DEL_WAIT was
// added back (heard again, imported) and goes; one not in the table is settled for that copy
void mesh_ctab_del_review(mesh_ctab_del_t *d, uint32_t now_s, bool (*in_table)(uint8_t const *key, void *ctx),
                          void *ctx);
// The card took the copy of the last review: the settled entries go
void mesh_ctab_del_settle(mesh_ctab_del_t *d);
// The list as a file ("LDEL", u16 format 1, u16 count, the keys, u32 checksum); the bytes, 0 when cap is too small
size_t mesh_ctab_del_pack(mesh_ctab_del_t const *d, uint8_t *buf, size_t cap);
// Replaces the list by a whole, undamaged file's (added_s = now_s); false and an empty list for anything else
bool mesh_ctab_del_unpack(mesh_ctab_del_t *d, uint8_t const *buf, size_t len, uint32_t now_s);

// ---- the card file ---------------------------------------------------------------------------------------------------
// Little-endian. Head (68 bytes, plain):
//   "LCON", u16 format 1, u16 record bytes (152), u32 generation, u32 records, u32 records per block (64),
//   salt (16), check (16), tag (16)
// then the records in blocks: AES-128-CTR(records) | tag (16). The keys: root = SHA-256("lichen-contacts-v1" | node
// private key), never stored; per file enc = HMAC(root, "enc" | salt)[0..16), mac = HMAC(root, "mac" | salt),
// check = HMAC(root, "check" | salt)[0..16). Every file gets a new salt, so a counter block (the block's number, then
// zeros, then the AES block index) is never used twice under one key. Head tag = HMAC(mac, "head" | head[0..52)),
// block tag = HMAC(mac, "blk" | u32 block number | ciphertext), both cut to 16 bytes (encrypt-then-MAC).
// As for the journal: a lost or copied card shows no names or keys, and a changed block is noticed. Someone who can
// write the card can still put an older file of this badge back; accepted, the list is a convenience copy.

#define MESH_CFILE_HEAD    68
#define MESH_CFILE_SALT    16
#define MESH_CFILE_TAG     16
#define MESH_CFILE_BLOCK   64 // records
#define MESH_CFILE_VERSION 1
#define MESH_CFILE_MAX     (MESH_CFILE_HEAD + MESH_CTAB_CAP_CARD * MESH_CTAB_REC + \
                            ((MESH_CTAB_CAP_CARD + MESH_CFILE_BLOCK - 1) / MESH_CFILE_BLOCK) * MESH_CFILE_TAG)

// root = SHA-256("lichen-contacts-v1" | prv)
bool   mesh_cfile_root(uint8_t root[32], uint8_t const *prv, size_t prv_len);
// The file's size for n records
size_t mesh_cfile_size(size_t n);
// The whole file for n records in out; 0 when cap is too small, n is over the cap or a primitive failed
size_t mesh_cfile_pack(uint8_t const root[32], uint8_t const salt[MESH_CFILE_SALT], uint32_t generation,
                       uint8_t const *recs, size_t n, uint8_t *out, size_t cap);

typedef enum {
    MESH_CFILE_OK,         // this badge's file: its good records are at the start of the buffer
    MESH_CFILE_UNREADABLE, // not a contact file of this format (short, another magic or version, sizes that do not fit)
    MESH_CFILE_FOREIGN,    // another identity's
    MESH_CFILE_DAMAGED,    // this badge's, but the head's tag fails or more than a tenth of its blocks do
    MESH_CFILE_CRYPTO,     // a primitive failed: no verdict
} mesh_cfile_verdict_t;

typedef struct {
    uint32_t generation;
    uint32_t records;    // as the head says
    uint32_t good;       // records read
    uint32_t blocks, bad_blocks; // a block cut off by a short file counts as bad
    bool     complete;   // every block is there and passed: the file is the whole list it was written as
} mesh_cfile_info_t;

// Checks and decrypts a file in place. On MESH_CFILE_OK buf starts with info->good plain records (the blocks that
// failed their tag left out); on anything else buf's contents are undefined.
mesh_cfile_verdict_t mesh_cfile_open(uint8_t const root[32], uint8_t *buf, size_t len, mesh_cfile_info_t *info);

// ---- what a start, or a card put in, does with the card ---------------------------------------------------------------

typedef enum {
    MESH_CTAB_CARD_NONE,    // no card, no key, or no verdict: /storage only, as before #472
    MESH_CTAB_CARD_USE,     // merge the file named by *use_side, then the card holds the list
    MESH_CTAB_CARD_FRESH,   // no contact file there: the card takes the list as it is
    MESH_CTAB_CARD_REFUSED, // a file of another identity or an unreadable one: left alone, /storage only
} mesh_ctab_card_t;

typedef struct {
    bool                 exists;
    mesh_cfile_verdict_t verdict;
    uint32_t             generation;
    bool                 complete; // mesh_cfile_info_t's
} mesh_ctab_file_t;

// bin: contacts.bin; side: contacts.new, the file a save writes before it takes the name (a cut can leave it as the
// only whole copy, or torn). A torn side file of this badge is ignored; a foreign file under either name refuses.
// A side file that lost blocks (a cut save can leave one that still passes the tenth rule) never wins over a readable
// contacts.bin: it is used only when there is no other.
mesh_ctab_card_t mesh_ctab_card_plan(bool card, bool key, mesh_ctab_file_t const *bin, mesh_ctab_file_t const *side,
                                     bool *use_side);

// Which file name a refused file is moved to by the owner's "Use this card for contacts": 0 contacts.old, 1
// contacts.old.1, -1 both exist (neither is ever overwritten: the owner clears the card first)
int mesh_ctab_aside_slot(bool have_0, bool have_1);

#ifdef __cplusplus
}
#endif
