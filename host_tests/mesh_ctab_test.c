// Host test: the contact list on the SD card (#472): the core set, the merge, the deleted keys, the card file
#include "mesh_ctab.h"
#include "mesh_hist_crypto.h"

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

#define N 300

static uint8_t  recs[N * MESH_CTAB_REC];
static uint8_t  keep[N];
static uint16_t order[N];

// Record i: key starts with i (two bytes), name "c<i>", a chat node, lastmod as given
static void rec_set(uint8_t *rec, unsigned i, uint32_t lastmod, bool fav) {
    memset(rec, 0, MESH_CTAB_REC);
    rec[0] = (uint8_t)i;
    rec[1] = (uint8_t)(i >> 8);
    rec[9] = 0x77;
    snprintf((char *)rec + MESH_CTAB_AT_NAME, MESH_CTAB_NAME, "c%u", i);
    rec[MESH_CTAB_AT_TYPE]  = 1;
    rec[MESH_CTAB_AT_FLAGS] = fav ? MESH_CTAB_FAV : 0;
    memcpy(rec + MESH_CTAB_AT_LASTMOD, &lastmod, 4); // the host is little-endian, as the badge
}

static void fill(size_t n) {
    for (size_t i = 0; i < n; i++) {
        rec_set(recs + i * MESH_CTAB_REC, (unsigned)i, 1000 + (uint32_t)i, false);
    }
}

static bool always_7_and_9(uint8_t const *rec, void *ctx) {
    (void)ctx;
    return rec[1] == 0 && (rec[0] == 7 || rec[0] == 9);
}

static bool always_all(uint8_t const *rec, void *ctx) {
    (void)rec;
    (void)ctx;
    return true;
}

static void test_select(void) {
    fill(20);
    // Fewer than the limit: everything stays
    CHECK(mesh_ctab_select(recs, 20, 250, NULL, NULL, keep, order) == 20);
    CHECK(keep[0] == MESH_CTAB_RECENT && keep[19] == MESH_CTAB_RECENT);

    // A limit of 5: the favourite (3), the two named ones (7, 9), then the two newest others (19, 18)
    recs[3 * MESH_CTAB_REC + MESH_CTAB_AT_FLAGS] = MESH_CTAB_FAV;
    CHECK(mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order) == 5);
    for (size_t i = 0; i < 20; i++) {
        uint8_t want = i == 3 || i == 7 || i == 9 ? MESH_CTAB_ALWAYS : i >= 18 ? MESH_CTAB_RECENT : MESH_CTAB_OUT;
        CHECK(keep[i] == want);
    }
    // The owner's own choices beyond the limit all stay, and no other
    CHECK(mesh_ctab_select(recs, 20, 2, always_7_and_9, NULL, keep, order) == 3);
    CHECK(keep[19] == MESH_CTAB_OUT && keep[3] == MESH_CTAB_ALWAYS);
    CHECK(mesh_ctab_select(recs, 20, 5, always_all, NULL, keep, order) == 20);
    // The same lastmod: the earlier record wins
    for (size_t i = 0; i < 20; i++) {
        rec_set(recs + i * MESH_CTAB_REC, (unsigned)i, 5, false);
    }
    CHECK(mesh_ctab_select(recs, 20, 4, NULL, NULL, keep, order) == 4);
    CHECK(keep[0] && keep[3] && !keep[4]);
    // Nothing at all
    CHECK(mesh_ctab_select(recs, 0, 4, NULL, NULL, keep, order) == 0);

    // A real size: 300 with shuffled times, the 250 newest stay
    for (size_t i = 0; i < N; i++) {
        rec_set(recs + i * MESH_CTAB_REC, (unsigned)i, (uint32_t)((i * 7919u) % N), false);
    }
    CHECK(mesh_ctab_select(recs, N, MESH_CTAB_CORE, NULL, NULL, keep, order) == MESH_CTAB_CORE);
    for (size_t i = 0; i < N; i++) {
        CHECK((keep[i] != MESH_CTAB_OUT) == (mesh_ctab_lastmod(recs + i * MESH_CTAB_REC) >= N - MESH_CTAB_CORE));
    }
    // Compacting keeps them in table order
    CHECK(mesh_ctab_compact(recs, N, keep) == MESH_CTAB_CORE);
    uint32_t last_index = 0;
    for (size_t i = 0; i < MESH_CTAB_CORE; i++) {
        uint8_t const *r     = recs + i * MESH_CTAB_REC;
        uint32_t       index = (uint32_t)(r[0] | (r[1] << 8));
        CHECK(mesh_ctab_lastmod(r) >= N - MESH_CTAB_CORE);
        CHECK(i == 0 || index > last_index);
        last_index = index;
    }
}

static void test_digest(void) {
    fill(20);
    recs[3 * MESH_CTAB_REC + MESH_CTAB_AT_FLAGS] = MESH_CTAB_FAV;
    mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order);
    uint32_t base = mesh_ctab_digest(recs, 20, keep);
    // What an advert changes in a member, and anything in a contact outside the group: no change
    uint32_t later = 99999;
    memcpy(recs + 3 * MESH_CTAB_REC + MESH_CTAB_AT_LASTMOD, &later, 4);
    recs[3 * MESH_CTAB_REC + 80] ^= 0xff; // its path
    recs[19 * MESH_CTAB_REC + MESH_CTAB_AT_NAME] = 'X';
    mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order);
    CHECK(mesh_ctab_digest(recs, 20, keep) == base);
    // A member renamed, a favourite more, a favourite less: each changes it
    recs[7 * MESH_CTAB_REC + MESH_CTAB_AT_NAME] = 'Z';
    mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order);
    uint32_t renamed = mesh_ctab_digest(recs, 20, keep);
    CHECK(renamed != base);
    recs[12 * MESH_CTAB_REC + MESH_CTAB_AT_FLAGS] = MESH_CTAB_FAV;
    mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order);
    uint32_t more = mesh_ctab_digest(recs, 20, keep);
    CHECK(more != renamed);
    recs[3 * MESH_CTAB_REC + MESH_CTAB_AT_FLAGS] = 0;
    mesh_ctab_select(recs, 20, 5, always_7_and_9, NULL, keep, order);
    CHECK(mesh_ctab_digest(recs, 20, keep) != more);
}

static void test_merge_plan(void) {
    CHECK(mesh_ctab_merge_plan(false, false, 0, 10) == MESH_CTAB_ADD);
    CHECK(mesh_ctab_merge_plan(true, false, 0, 10) == MESH_CTAB_SKIP);      // deleted stays deleted
    CHECK(mesh_ctab_merge_plan(false, true, 10, 11) == MESH_CTAB_REPLACE);  // the newer wins
    CHECK(mesh_ctab_merge_plan(false, true, 10, 10) == MESH_CTAB_SKIP);     // a duplicate
    CHECK(mesh_ctab_merge_plan(false, true, 11, 10) == MESH_CTAB_SKIP);
    CHECK(mesh_ctab_merge_plan(true, true, 10, 11) == MESH_CTAB_REPLACE);   // added back since: an ordinary contact
}

static uint8_t table_has[4]; // first key bytes the "table" holds

static bool in_table(uint8_t const *key, void *ctx) {
    (void)ctx;
    return memchr(table_has, key[0], sizeof(table_has)) != NULL && key[0] != 0;
}

static void test_deleted(void) {
    static mesh_ctab_del_t d, e;
    uint8_t                key[32] = {0}, file[MESH_CTAB_DEL_FILE];
    key[0] = 1;
    mesh_ctab_del_add(&d, key, 100);
    key[0] = 2;
    mesh_ctab_del_add(&d, key, 100);
    CHECK(d.n == 2 && mesh_ctab_del_has(&d, key));
    mesh_ctab_del_add(&d, key, 130); // again: no second entry
    CHECK(d.n == 2);
    key[0] = 3;
    CHECK(!mesh_ctab_del_has(&d, key));

    // The file, and what is not one
    size_t len = mesh_ctab_del_pack(&d, file, sizeof(file));
    CHECK(len == 12 + 2 * MESH_CTAB_DEL_KEY);
    CHECK(mesh_ctab_del_pack(&d, file, len - 1) == 0);
    CHECK(mesh_ctab_del_unpack(&e, file, len, 7) && e.n == 2 && e.added_s[1] == 7);
    key[0] = 2;
    CHECK(mesh_ctab_del_has(&e, key));
    file[9] ^= 1;
    CHECK(!mesh_ctab_del_unpack(&e, file, len, 7) && e.n == 0);
    file[9] ^= 1;
    CHECK(!mesh_ctab_del_unpack(&e, file, len - 1, 7));
    CHECK(!mesh_ctab_del_unpack(&e, file, 0, 7));

    // 1 is gone from the table (settled); 2 is still there: within the wait it may simply not be removed yet
    table_has[0] = 2;
    mesh_ctab_del_review(&d, 140, in_table, NULL);
    CHECK(d.n == 2 && d.settled[0] && !d.settled[1]);
    // The card took that copy: 1 is forgotten, 2 stays
    uint32_t changes = d.changes;
    mesh_ctab_del_settle(&d);
    CHECK(d.n == 1 && d.key[0][0] == 2 && d.changes != changes);
    // Still in the table after the wait: it was added back, the entry goes
    mesh_ctab_del_review(&d, 130 + MESH_CTAB_DEL_WAIT, in_table, NULL);
    CHECK(d.n == 0);

    // A full list forgets its oldest
    for (unsigned i = 0; i < MESH_CTAB_DEL + 1; i++) {
        key[0] = (uint8_t)(10 + i);
        mesh_ctab_del_add(&d, key, 1);
    }
    CHECK(d.n == MESH_CTAB_DEL);
    key[0] = 10;
    CHECK(!mesh_ctab_del_has(&d, key));
    key[0] = 11;
    CHECK(mesh_ctab_del_has(&d, key));
    key[0] = 10 + MESH_CTAB_DEL;
    CHECK(mesh_ctab_del_has(&d, key));
    CHECK(mesh_ctab_del_pack(&d, file, sizeof(file)) == MESH_CTAB_DEL_FILE);
}

static uint8_t root[32], other_root[32], file[MESH_CFILE_HEAD + N * MESH_CTAB_REC + 5 * MESH_CFILE_TAG + 8];
static uint8_t plain[N * MESH_CTAB_REC], work[sizeof(file)];
static uint8_t const SALT[MESH_CFILE_SALT] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static mesh_cfile_verdict_t open_copy(uint8_t const *key, size_t len, mesh_cfile_info_t *info) {
    memcpy(work, file, sizeof(file));
    return mesh_cfile_open(key, work, len, info);
}

static void test_file(void) {
    uint8_t           prv[64], prv2[64];
    mesh_cfile_info_t info;
    memset(prv, 0x11, sizeof(prv));
    memset(prv2, 0x12, sizeof(prv2));
    CHECK(mesh_cfile_root(root, prv, sizeof(prv)) && mesh_cfile_root(other_root, prv2, sizeof(prv2)));
    CHECK(memcmp(root, other_root, 32) != 0);
    CHECK(!mesh_cfile_root(root + 0, NULL, 0));
    for (size_t i = 0; i < N; i++) {
        rec_set(plain + i * MESH_CTAB_REC, (unsigned)i, 5000 + (uint32_t)i, i % 9 == 0);
    }

    // 300 records: 5 blocks (4 of 64, one of 44)
    size_t len = mesh_cfile_pack(root, SALT, 42, plain, N, file, sizeof(file));
    CHECK(len == mesh_cfile_size(N) && len == MESH_CFILE_HEAD + N * MESH_CTAB_REC + 5 * MESH_CFILE_TAG);
    CHECK(mesh_cfile_pack(root, SALT, 42, plain, N, work, len - 1) == 0);
    CHECK(mesh_cfile_pack(root, SALT, 42, plain, MESH_CTAB_CAP_CARD + 1, work, sizeof(work)) == 0);
    // Nothing of a record is readable in the file: neither a name nor a key
    bool leaked = false;
    for (size_t i = MESH_CFILE_HEAD; i + 4 <= len; i++) {
        leaked = leaked || memcmp(file + i, "c12", 4) == 0;
    }
    CHECK(!leaked);
    // The round trip
    CHECK(open_copy(root, len, &info) == MESH_CFILE_OK);
    CHECK(info.generation == 42 && info.records == N && info.good == N && info.blocks == 5 && info.bad_blocks == 0);
    CHECK(info.complete);
    CHECK(memcmp(work, plain, sizeof(plain)) == 0);
    // Trailing bytes change nothing
    CHECK(open_copy(root, len + 8, &info) == MESH_CFILE_OK && info.good == N);
    // Another identity
    CHECK(open_copy(other_root, len, &info) == MESH_CFILE_FOREIGN);
    // Not a contact file
    CHECK(open_copy(root, MESH_CFILE_HEAD - 1, &info) == MESH_CFILE_UNREADABLE);
    file[0] ^= 1;
    CHECK(open_copy(root, len, &info) == MESH_CFILE_UNREADABLE);
    file[0] ^= 1;
    file[4] = 2; // a later format
    CHECK(open_copy(root, len, &info) == MESH_CFILE_UNREADABLE);
    file[4] = 1;
    // A head field changed (the count): ours, but damaged
    file[12] ^= 1;
    CHECK(open_copy(root, len, &info) == MESH_CFILE_DAMAGED);
    file[12] ^= 1;
    file[8] ^= 1; // the generation
    CHECK(open_copy(root, len, &info) == MESH_CFILE_DAMAGED);
    file[8] ^= 1;

    // A flipped byte in each block in turn: that block's records are lost, the others read; 1 of 5 is over a tenth
    size_t stride = MESH_CFILE_BLOCK * MESH_CTAB_REC + MESH_CFILE_TAG;
    for (size_t b = 0; b < 5; b++) {
        size_t at = MESH_CFILE_HEAD + b * stride + 100;
        file[at] ^= 0x40;
        CHECK(open_copy(root, len, &info) == MESH_CFILE_DAMAGED);
        CHECK(info.bad_blocks == 1 && info.good == N - (b == 4 ? 44 : 64));
        file[at] ^= 0x40;
    }
    // A flipped byte in a block's tag, and in the head's salt and check: the block is lost; the file is not ours
    file[MESH_CFILE_HEAD + stride - 1] ^= 1;
    CHECK(open_copy(root, len, &info) == MESH_CFILE_DAMAGED && info.bad_blocks == 1);
    file[MESH_CFILE_HEAD + stride - 1] ^= 1;
    file[20] ^= 1;
    CHECK(open_copy(root, len, &info) == MESH_CFILE_FOREIGN);
    file[20] ^= 1;
    file[36] ^= 1;
    CHECK(open_copy(root, len, &info) == MESH_CFILE_FOREIGN);
    file[36] ^= 1;
    file[13] = 0x10; // 4140 records: more than the table ever takes
    CHECK(open_copy(root, len, &info) == MESH_CFILE_UNREADABLE);
    file[13] = 1;
    // Two blocks swapped: both fail (the tag names the block's place)
    memcpy(work, file + MESH_CFILE_HEAD, stride);
    memcpy(file + MESH_CFILE_HEAD, file + MESH_CFILE_HEAD + stride, stride);
    memcpy(file + MESH_CFILE_HEAD + stride, work, stride);
    CHECK(open_copy(root, len, &info) == MESH_CFILE_DAMAGED && info.bad_blocks == 2);
    // A cut file: the last block is gone
    mesh_cfile_pack(root, SALT, 42, plain, N, file, sizeof(file));
    CHECK(open_copy(root, len - 1, &info) == MESH_CFILE_DAMAGED && info.bad_blocks == 1 && info.good == N - 44);
    CHECK(open_copy(root, MESH_CFILE_HEAD, &info) == MESH_CFILE_DAMAGED && info.bad_blocks == 5 && info.good == 0);

    // A bigger file with one bad block of 16 stays usable: the good records, in order, without the bad block's
#define BIG 1000
    static uint8_t big_plain[BIG * MESH_CTAB_REC], big[MESH_CFILE_HEAD + BIG * MESH_CTAB_REC + 16 * MESH_CFILE_TAG];
    for (size_t i = 0; i < BIG; i++) {
        rec_set(big_plain + i * MESH_CTAB_REC, (unsigned)i, (uint32_t)i, false);
    }
    size_t big_len = mesh_cfile_pack(root, SALT, 7, big_plain, BIG, big, sizeof(big));
    CHECK(big_len == sizeof(big));
    big[MESH_CFILE_HEAD + 2 * stride + 5] ^= 1;
    CHECK(mesh_cfile_open(root, big, big_len, &info) == MESH_CFILE_OK);
    CHECK(info.blocks == 16 && info.bad_blocks == 1 && info.good == BIG - 64 && !info.complete);
    CHECK(memcmp(big, big_plain, 128 * MESH_CTAB_REC) == 0);
    CHECK(memcmp(big + 128 * MESH_CTAB_REC, big_plain + 192 * MESH_CTAB_REC, (BIG - 192) * MESH_CTAB_REC) == 0);
    // The same file cut inside its last block (a save cut by a power loss): readable, 1 of 16 lost, and not complete
    big_len = mesh_cfile_pack(root, SALT, 7, big_plain, BIG, big, sizeof(big));
    CHECK(mesh_cfile_open(root, big, big_len - 20, &info) == MESH_CFILE_OK);
    CHECK(info.bad_blocks == 1 && info.good == 960 && !info.complete);

    // The tenth rule's edge: 1 bad block of 10 is still a list (640 records), 2 of 10 is not
    big_len = mesh_cfile_pack(root, SALT, 7, big_plain, 640, big, sizeof(big));
    big[MESH_CFILE_HEAD + 5] ^= 1;
    CHECK(mesh_cfile_open(root, big, big_len, &info) == MESH_CFILE_OK && info.blocks == 10 && info.good == 576);
    big_len = mesh_cfile_pack(root, SALT, 7, big_plain, 640, big, sizeof(big));
    big[MESH_CFILE_HEAD + 5] ^= 1;
    big[MESH_CFILE_HEAD + stride + 5] ^= 1;
    CHECK(mesh_cfile_open(root, big, big_len, &info) == MESH_CFILE_DAMAGED);

    // An empty list is a file too
    len = mesh_cfile_pack(root, SALT, 1, plain, 0, file, sizeof(file));
    CHECK(len == MESH_CFILE_HEAD && open_copy(root, len, &info) == MESH_CFILE_OK && info.good == 0);
    // Another salt gives another file for the same records
    mesh_cfile_pack(root, SALT, 1, plain, 10, file, sizeof(file));
    uint8_t salt2[MESH_CFILE_SALT] = {9};
    mesh_cfile_pack(root, salt2, 1, plain, 10, work, sizeof(work));
    CHECK(memcmp(file + MESH_CFILE_HEAD, work + MESH_CFILE_HEAD, 64) != 0);

    // A primitive that fails gives no file and no verdict
    mesh_cfile_pack(root, SALT, 1, plain, 10, file, sizeof(file));
    mh_test_fail(true);
    CHECK(mesh_cfile_pack(root, SALT, 1, plain, 10, work, sizeof(work)) == 0);
    CHECK(open_copy(root, mesh_cfile_size(10), &info) == MESH_CFILE_CRYPTO);
    CHECK(!mesh_cfile_root(work, prv, sizeof(prv)));
    mh_test_fail(false);
}

static bool side_lost_blocks; // the side file reads, but not all of it

static mesh_ctab_card_t plan(bool card, bool key, int bin, uint32_t bin_gen, int side, uint32_t side_gen,
                             bool *use_side) {
    mesh_ctab_file_t b = {bin >= 0, (mesh_cfile_verdict_t)(bin < 0 ? 0 : bin), bin_gen, bin == MESH_CFILE_OK};
    mesh_ctab_file_t s = {side >= 0, (mesh_cfile_verdict_t)(side < 0 ? 0 : side), side_gen,
                          side == MESH_CFILE_OK && !side_lost_blocks};
    return mesh_ctab_card_plan(card, key, &b, &s, use_side);
}

static void test_card_plan(void) {
    bool side = true;
    enum { NO = -1 };
    CHECK(plan(false, true, MESH_CFILE_OK, 1, NO, 0, &side) == MESH_CTAB_CARD_NONE && !side);
    CHECK(plan(true, false, MESH_CFILE_OK, 1, NO, 0, &side) == MESH_CTAB_CARD_NONE);
    // A card without a contact file takes the list
    CHECK(plan(true, true, NO, 0, NO, 0, &side) == MESH_CTAB_CARD_FRESH);
    // Our file
    CHECK(plan(true, true, MESH_CFILE_OK, 5, NO, 0, &side) == MESH_CTAB_CARD_USE && !side);
    // A cut between the remove and the rename left only the side file, whole
    CHECK(plan(true, true, NO, 0, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && side);
    // Both whole: the newer
    CHECK(plan(true, true, MESH_CFILE_OK, 5, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && side);
    CHECK(plan(true, true, MESH_CFILE_OK, 6, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && !side);
    // A side file cut while it was written is ignored, with and without a file beside it
    CHECK(plan(true, true, MESH_CFILE_OK, 5, MESH_CFILE_DAMAGED, 6, &side) == MESH_CTAB_CARD_USE && !side);
    CHECK(plan(true, true, NO, 0, MESH_CFILE_DAMAGED, 1, &side) == MESH_CTAB_CARD_FRESH);
    CHECK(plan(true, true, NO, 0, MESH_CFILE_UNREADABLE, 0, &side) == MESH_CTAB_CARD_FRESH);
    // A side file that reads but lost blocks (a cut save, within the tenth rule) never beats a readable contacts.bin,
    // however new; alone, or next to our damaged file, it is the best there is
    side_lost_blocks = true;
    CHECK(plan(true, true, MESH_CFILE_OK, 5, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && !side);
    CHECK(plan(true, true, NO, 0, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && side);
    CHECK(plan(true, true, MESH_CFILE_DAMAGED, 5, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && side);
    side_lost_blocks = false;
    // Another badge's file, or one that cannot be read: left alone
    CHECK(plan(true, true, MESH_CFILE_FOREIGN, 5, NO, 0, &side) == MESH_CTAB_CARD_REFUSED);
    CHECK(plan(true, true, MESH_CFILE_UNREADABLE, 0, NO, 0, &side) == MESH_CTAB_CARD_REFUSED);
    CHECK(plan(true, true, MESH_CFILE_DAMAGED, 5, NO, 0, &side) == MESH_CTAB_CARD_REFUSED);
    CHECK(plan(true, true, MESH_CFILE_FOREIGN, 5, MESH_CFILE_OK, 9, &side) == MESH_CTAB_CARD_REFUSED && !side);
    CHECK(plan(true, true, MESH_CFILE_OK, 5, MESH_CFILE_FOREIGN, 9, &side) == MESH_CTAB_CARD_REFUSED);
    // Our damaged file with a whole side file next to it: the side file is the list
    CHECK(plan(true, true, MESH_CFILE_DAMAGED, 5, MESH_CFILE_OK, 6, &side) == MESH_CTAB_CARD_USE && side);
    // No verdict: nothing is decided about the card this time
    CHECK(plan(true, true, MESH_CFILE_CRYPTO, 0, NO, 0, &side) == MESH_CTAB_CARD_NONE);
    CHECK(plan(true, true, MESH_CFILE_OK, 5, MESH_CFILE_CRYPTO, 0, &side) == MESH_CTAB_CARD_NONE);

    CHECK(mesh_ctab_aside_slot(false, false) == 0 && mesh_ctab_aside_slot(true, false) == 1);
    CHECK(mesh_ctab_aside_slot(true, true) == -1 && mesh_ctab_aside_slot(false, true) == 0);
}

static void test_defer(void) {
    enum { NR = 3 };
    uint8_t a[NR * MESH_CTAB_REC], b[NR * MESH_CTAB_REC];
    memset(a, 0x11, sizeof(a));
    memcpy(b, a, sizeof(a));
    CHECK(mesh_ctab_diff(a, b, NR) == MESH_CTAB_SAME);
    // the advert time and lastmod of one record, every byte of both
    for (int i = 0; i < 4; i++) {
        b[MESH_CTAB_REC + MESH_CTAB_AT_ADVERT + i] ^= 0xff;
        b[2 * MESH_CTAB_REC + MESH_CTAB_AT_LASTMOD + i] ^= 0xff;
    }
    CHECK(mesh_ctab_diff(a, b, NR) == MESH_CTAB_TIMES_ONLY);
    // every other byte of a record is a real change, the bytes next to the timestamps included
    for (int i = 0; i < MESH_CTAB_REC; i++) {
        bool ts = (i >= MESH_CTAB_AT_ADVERT && i < MESH_CTAB_AT_ADVERT + 4) ||
                  (i >= MESH_CTAB_AT_LASTMOD && i < MESH_CTAB_AT_LASTMOD + 4);
        memcpy(b, a, sizeof(a));
        b[MESH_CTAB_REC + i] ^= 1;
        CHECK(mesh_ctab_diff(a, b, NR) == (ts ? MESH_CTAB_TIMES_ONLY : MESH_CTAB_CHANGED));
    }
    // a timestamp change and a name change together: changed
    memcpy(b, a, sizeof(a));
    b[MESH_CTAB_AT_LASTMOD] ^= 1;
    b[2 * MESH_CTAB_REC + MESH_CTAB_AT_NAME] ^= 1;
    CHECK(mesh_ctab_diff(a, b, NR) == MESH_CTAB_CHANGED);

    uint32_t last = 1000, iv = MESH_CTAB_SAVE_INTERVAL_S;
    CHECK(iv == 900);
    CHECK(mesh_ctab_defer(MESH_CTAB_TIMES_ONLY, false, last + 1, last));
    CHECK(mesh_ctab_defer(MESH_CTAB_TIMES_ONLY, false, last + iv - 1, last));
    CHECK(!mesh_ctab_defer(MESH_CTAB_TIMES_ONLY, false, last + iv, last));  // the interval is up
    CHECK(!mesh_ctab_defer(MESH_CTAB_TIMES_ONLY, true, last + 1, last));    // a stop or the owner
    CHECK(!mesh_ctab_defer(MESH_CTAB_CHANGED, false, last + 1, last));
    CHECK(!mesh_ctab_defer(MESH_CTAB_SAME, false, last + 1, last));
    CHECK(!mesh_ctab_flush_due(last + iv - 1, last) && mesh_ctab_flush_due(last + iv, last));
    CHECK(mesh_ctab_flush_due(5, last)); // the uptime clock cannot go back: if it did, write
}

typedef struct {
    uint8_t const *file;
    size_t         len;
    int            reads, fail_at;
} fake_t;

static size_t fake_read(void *ctx, size_t at, uint8_t *out, size_t want) {
    fake_t *f = ctx;
    if (f->reads++ == f->fail_at || at + want > f->len) {
        return 0;
    }
    memcpy(out, f->file + at, want);
    return want;
}

static int diff_calls;
static mesh_ctab_diff_t fake_diff(void *ctx) {
    diff_calls++;
    return *(mesh_ctab_diff_t *)ctx;
}

static void test_file_diff(void) {
    enum { NR = 60 }; // 9120 B: two full 26-record steps and a short last one of 8
    static uint8_t file[NR * MESH_CTAB_REC], save[NR * MESH_CTAB_REC], scratch[4096];
    memset(file, 0x22, sizeof(file));
    memcpy(save, file, sizeof(file));
    fake_t f = {file, sizeof(file), 0, -1};
    size_t L = sizeof(file);
    CHECK(mesh_ctab_file_diff(save, L, true, L, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_SAME && f.reads == 3);
    save[(NR - 1) * MESH_CTAB_REC + MESH_CTAB_AT_LASTMOD] ^= 1; // in the short last chunk
    f.reads = 0;
    CHECK(mesh_ctab_file_diff(save, L, true, L, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_TIMES_ONLY);
    save[5 * MESH_CTAB_REC + MESH_CTAB_AT_NAME] ^= 1; // in the first
    f.reads = 0;
    CHECK(mesh_ctab_file_diff(save, L, true, L, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_CHANGED && f.reads == 1);
    save[5 * MESH_CTAB_REC + MESH_CTAB_AT_NAME] ^= 1;
    CHECK(mesh_ctab_file_diff(save, L, false, 0, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_CHANGED); // missing
    CHECK(mesh_ctab_file_diff(save, L, true, L - MESH_CTAB_REC, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_CHANGED);
    CHECK(mesh_ctab_file_diff(save, L - 1, true, L - 1, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_CHANGED);
    f.reads = 0, f.fail_at = 1; // a read that fails in the second step
    CHECK(mesh_ctab_file_diff(save, L, true, L, fake_read, &f, scratch, sizeof(scratch)) == MESH_CTAB_CHANGED);
    f.fail_at = -1;
    CHECK(mesh_ctab_file_diff(save, L, true, L, fake_read, &f, scratch, 100) == MESH_CTAB_CHANGED); // no room for a record

    // park: a forced write or a due one never asks for the difference; deferral, release, write, then parked again
    mesh_ctab_diff_t times = MESH_CTAB_TIMES_ONLY;
    uint32_t last = 100;
    diff_calls    = 0;
    CHECK(!mesh_ctab_park(true, last + 1, last, fake_diff, &times) && diff_calls == 0);
    CHECK(mesh_ctab_park(false, last + 10, last, fake_diff, &times) && diff_calls == 1);
    CHECK(mesh_ctab_park(false, last + MESH_CTAB_SAVE_INTERVAL_S - 1, last, fake_diff, &times));
    diff_calls = 0;
    CHECK(!mesh_ctab_park(false, last + MESH_CTAB_SAVE_INTERVAL_S, last, fake_diff, &times) && diff_calls == 0);
    last += MESH_CTAB_SAVE_INTERVAL_S; // the write took place: the interval starts again
    CHECK(mesh_ctab_park(false, last + 10, last, fake_diff, &times));
    mesh_ctab_diff_t changed = MESH_CTAB_CHANGED;
    CHECK(!mesh_ctab_park(false, last + 10, last, fake_diff, &changed));
}

static mesh_ctab_diffstat_t stat_new(void) {
    mesh_ctab_diffstat_t st = {0};
    st.complete             = true;
    return st;
}

// The merge's accounting over a card file (cards) and a table (tab) with the real plan; table_after contacts are in
// the table afterwards; the card record whose key starts with deleted_id was deleted by hand (0xFFFF: none)
static mesh_ctab_diffstat_t run_merge(uint8_t const *cards, size_t nc, uint8_t const *tab, size_t nt, size_t table_after,
                                      unsigned deleted_id) {
    mesh_ctab_diffstat_t st = stat_new();
    for (size_t i = 0; i < nc; i++) {
        uint8_t const *c = cards + i * MESH_CTAB_REC;
        uint8_t const *t = NULL;
        for (size_t j = 0; j < nt; j++) {
            if (memcmp(tab + j * MESH_CTAB_REC, c, MESH_CTAB_PUB) == 0) {
                t = tab + j * MESH_CTAB_REC;
            }
        }
        bool              dup  = false;
        for (size_t k = 0; t && k < i; k++) { // an earlier card record with this key
            dup = dup || memcmp(cards + k * MESH_CTAB_REC, c, MESH_CTAB_PUB) == 0;
        }
        bool              del  = c[0] == deleted_id;
        mesh_ctab_merge_t plan = mesh_ctab_merge_plan(del, t != NULL, t ? mesh_ctab_lastmod(t) : 0, mesh_ctab_lastmod(c));
        mesh_ctab_account(&st, plan, c[MESH_CTAB_AT_TYPE] == 0, dup, t, c);
    }
    mesh_ctab_account_done(&st, table_after);
    return st;
}

static bool need(mesh_ctab_diffstat_t st) {
    return mesh_ctab_card_write_needed(&st);
}

static void test_hold(void) {
    mesh_ctab_hold_t h = {.merging = true, .card_mounted = true};
    CHECK(mesh_ctab_hold_internal(&h)); // merging: /storage waits
    h.merging = false;
    CHECK(!mesh_ctab_hold_internal(&h)); // merge through
    h.card_pending = true;
    CHECK(mesh_ctab_hold_internal(&h)); // a card not looked at yet
    h.card_mounted = false;
    CHECK(!mesh_ctab_hold_internal(&h)); // pulled
    h.card_mounted = true;
    h.card_failed  = true;
    CHECK(!mesh_ctab_hold_internal(&h)); // failed: /storage keeps everything
    h.card_failed  = false;
    h.card_current = true;
    CHECK(!mesh_ctab_hold_internal(&h)); // the card already holds the table
    h.card_current   = false;
    h.pending_age_s  = MESH_CTAB_PENDING_MAX_S - 1;
    CHECK(mesh_ctab_hold_internal(&h)); // still within the limit
    h.pending_age_s = MESH_CTAB_PENDING_MAX_S;
    CHECK(!mesh_ctab_hold_internal(&h)); // a probe that keeps failing: /storage writes
    h.merging = true;
    CHECK(mesh_ctab_hold_internal(&h)); // a merge in progress holds whatever the age
    // a dropped save is owed once the hold ends, and not before
    CHECK(mesh_ctab_save_owed(true, false) && !mesh_ctab_save_owed(true, true) && !mesh_ctab_save_owed(false, false));
}

static void test_write_needed(void) {
    uint8_t card[2 * MESH_CTAB_REC], tab[2 * MESH_CTAB_REC];
    rec_set(card, 1, 10, false);
    rec_set(card + MESH_CTAB_REC, 2, 10, false);
    memcpy(tab, card, sizeof(tab));
    CHECK(!need(run_merge(card, 2, tab, 2, 2, 0xFFFF))); // the same list: nothing to write
    CHECK(!need(run_merge(card, 2, tab, 1, 2, 0xFFFF))); // a subset with equal timestamps: the table becomes the card's
    // Card {1,2}; 2 deleted by hand while it was out, 3 only on /storage: {1,3} has as many contacts as the card
    rec_set(tab + MESH_CTAB_REC, 3, 10, false);
    mesh_ctab_diffstat_t st = run_merge(card, 2, tab, 2, 2, 2);
    CHECK(st.card_only == 1 && st.table_only == 1 && mesh_ctab_card_write_needed(&st));
    memcpy(tab, card, MESH_CTAB_REC); // only the deleted card contact: the table lacks it
    st = run_merge(card, 2, tab, 1, 1, 2);
    CHECK(st.card_only == 1 && mesh_ctab_card_write_needed(&st));
    memcpy(tab, card, sizeof(tab)); // equal lastmod, the table's favourite flag differs
    tab[MESH_CTAB_AT_FLAGS] |= MESH_CTAB_FAV;
    st = run_merge(card, 2, tab, 2, 2, 0xFFFF);
    CHECK(st.table_only == 1 && mesh_ctab_card_write_needed(&st));
    memcpy(tab, card, sizeof(tab)); // the table's record is newer
    rec_set(tab, 1, 20, false);
    CHECK(need(run_merge(card, 2, tab, 2, 2, 0xFFFF)));
    rec_set(tab, 1, 5, false); // the card's is newer: the table takes it and equals the card
    CHECK(!need(run_merge(card, 2, tab, 2, 2, 0xFFFF)));
    CHECK(need(run_merge(card, 2, tab, 2, 3, 0xFFFF))); // a contact only /storage had
    st       = stat_new();
    st.fresh = true; // a new list
    CHECK(mesh_ctab_card_write_needed(&st));
    st          = stat_new();
    st.complete = false; // a file that lost blocks
    CHECK(mesh_ctab_card_write_needed(&st));
    // The card repeats key 1 and the table has {1, 3}: 3 is on neither medium unless the card is written
    uint8_t dupc[2 * MESH_CTAB_REC], dupt[2 * MESH_CTAB_REC];
    rec_set(dupc, 1, 10, false);
    memcpy(dupc + MESH_CTAB_REC, dupc, MESH_CTAB_REC);
    rec_set(dupt, 1, 10, false);
    rec_set(dupt + MESH_CTAB_REC, 3, 10, false);
    st = run_merge(dupc, 2, dupt, 2, 2, 0xFFFF);
    CHECK(st.table_only == 1 && st.card_only == 1 && mesh_ctab_card_write_needed(&st));
    // A type-none record on the card is never in the table (getNumContacts leaves such slots out): rewritten without it
    uint8_t nonec[MESH_CTAB_REC];
    rec_set(nonec, 4, 10, false);
    nonec[MESH_CTAB_AT_TYPE] = 0;
    CHECK(need(run_merge(nonec, 1, NULL, 0, 0, 0xFFFF)));
    st = run_merge(card, 2, tab, 0, 1, 0xFFFF); // a card record without room
    mesh_ctab_account_full(&st);
    CHECK(mesh_ctab_card_write_needed(&st));
}

int main(void) {
    test_select();
    test_digest();
    test_merge_plan();
    test_deleted();
    test_file();
    test_card_plan();
    test_defer();
    test_file_diff();
    test_hold();
    test_write_needed();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_ctab_test: ok\n");
    return 0;
}
