#include "mesh_ctab_run.h"

#include "mesh_int.h"

#include "fw_console.h"
#include "lichen_fs.h"
#include "lichen_mem.h"
#include "lichen_notify.h"
#include "lichen_random.h"
#include "mesh.h"
#include "mesh_advtab.h"
#include "mesh_hist_crypto.h"
#include "mesh_store.h"
#include "meshcore_run.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace mesh_int;

static char const TAG[] = "mesh_contacts";

#define SD_ROOT        "/sd/lichen"
#define SD_DIR         SD_ROOT "/meshcore"
#define BIN_PATH       SD_DIR "/contacts.bin"
#define SIDE_PATH      SD_DIR "/contacts.new" // a save writes here first, then it takes the name
#define OLD_PATH       SD_DIR "/contacts.old" // a file this badge cannot read, moved aside by the owner
#define OLD1_PATH      SD_DIR "/contacts.old.1"
#define DEL_NAME       "contacts.del"
#define DEL_SIDE_NAME  "contacts.del.new"
#define PATH_MAX_LEN   160
#define BOOT_WAIT_MS   5000
#define WRITE_WAIT_MS  100 // the card's batch lock: when another batch has it, a later loop tries again
#define STOP_WAIT_MS   3000
#define RETRY_S        5
#define SEEN_BITS      65536 // the merge's first-look filter for repeated keys
#define MERGE_PER_LOOP 64   // card records merged per loop of the mesh task: each is a walk over the table
#define CHUNK          4096 // the read-back check's buffer
#define PW_BUF         96

static BadgeMesh *nd;                    // the table's owner; set at boot, before the task
static char       dir[PATH_MAX_LEN - 24]; // MeshCore's files on /storage
static uint8_t    root[32];              // the card file's root key (mesh_ctab.h), wiped at the final stop
static bool       have_key;

// Read by any task (words): the cap by whoever adds a contact under the mesh lock
static int         cap_now   = MESH_CTAB_CAP_INTERNAL;
static int         place_now = MESH_CTAB_ON_STORAGE;
static bool        owner_dirty, use_request;
static char const *use_error;

// The mesh task's own
static uint8_t *merge;      // PSRAM: a card file's records waiting to go into the table
static size_t   merge_n, merge_at;
static bool     merge_fresh; // a card without a contact file: nothing to merge, the card takes the list
static bool     mounted_seen, card_current, card_dirty, card_force, card_take, want_save, wipe, stopping;
static bool     notice_pending, settle_pending;
static bool     keep_old; // the last merge left card contacts out (a full table): the next save keeps the file aside
static uint32_t merge_full_before;
static uint32_t generation, last_card_try_s, retry_at_s, last_internal_s, last_digest;
static bool     have_digest;
static bool     card_failed; // the last card write failed
static mesh_ctab_diffstat_t merge_stat; // what the merge in progress saw of the card's file against the table
static bool     save_held; // a /storage save was dropped by the hold: made up when the hold ends, whatever the outcome
static uint8_t *seen_bits; // PSRAM, during a merge only: bit set = a key with this hash was merged already
static uint32_t pending_since_s; // when the card was first seen mounted and not looked at (0: not pending)
static bool     deferred; // a save of timestamp changes only stays parked in the shim (#429)

// The deleted keys: touched only with the mesh lock (or before the task exists)
static mesh_ctab_del_t del;
static uint32_t        del_saved;
static uint8_t         del_file[MESH_CTAB_DEL_FILE];
static size_t          del_file_len;
static bool            del_write_pending;

// Scratch, PSRAM, allocated once at boot
static uint8_t     *keep;
static uint16_t    *order;
static mesh_conv_t *convs;
static size_t       convs_n;
static uint8_t     *chunk;

// Console figures (words; a stale value is fine)
static volatile uint32_t card_writes, card_errors, card_last_ms, card_last_bytes, card_bad_blocks;
static volatile uint32_t internal_writes, internal_skips, internal_parked, internal_kept, merged_added, merged_newer, merged_full;

static void *psram(size_t bytes) {
    return lichen_alloc(meshcore_ctx(), bytes, LICHEN_MEM_BULK);
}

static void unalloc(void *p) {
    lichen_free(meshcore_ctx(), p);
}

static void set_place(mesh_ctab_place_t p) {
    __atomic_store_n(&place_now, (int)p, __ATOMIC_RELEASE);
    if (p != MESH_CTAB_CARD_LEFT_ALONE) {
        __atomic_store_n(&use_error, (char const *)NULL, __ATOMIC_RELEASE); // it was about a file left alone
    }
    __atomic_store_n(&cap_now, p == MESH_CTAB_ON_CARD ? MESH_CTAB_CAP_CARD : MESH_CTAB_CAP_INTERNAL, __ATOMIC_RELEASE);
}

int mesh_ctab_run_cap(void) {
    return __atomic_load_n(&cap_now, __ATOMIC_ACQUIRE);
}

mesh_ctab_place_t mesh_ctab_run_place(void) {
    return (mesh_ctab_place_t)__atomic_load_n(&place_now, __ATOMIC_ACQUIRE);
}

void mesh_ctab_run_use_card(void) {
    __atomic_store_n(&use_error, (char const *)NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&use_request, true, __ATOMIC_RELEASE);
}

char const *mesh_ctab_run_use_card_error(void) {
    return __atomic_load_n(&use_error, __ATOMIC_ACQUIRE);
}

void mesh_ctab_run_owner_change(void) {
    __atomic_store_n(&owner_dirty, true, __ATOMIC_RELEASE);
}

void mesh_ctab_run_deleted_locked(uint8_t const *pub) {
    mesh_ctab_del_add(&del, pub, mesh_uptime_s());
    mesh_ctab_run_owner_change();
}

static void drop_merge(void) {
    unalloc(merge);
    unalloc(seen_bits);
    seen_bits   = NULL;
    merge       = NULL;
    merge_n     = merge_at = 0;
    merge_fresh = false;
}

static bool exists(char const *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// ---- reading the card ------------------------------------------------------------------------------------------------

// The file's bytes (PSRAM, the caller frees); NULL with *exists for one too short, too long or not read to its end
static uint8_t *read_file(char const *path, size_t *len, bool *exists) {
    struct stat st;
    *exists = stat(path, &st) == 0;
    *len    = 0;
    if (!*exists || st.st_size < MESH_CFILE_HEAD || st.st_size > MESH_CFILE_MAX) {
        return NULL;
    }
    uint8_t *buf = (uint8_t *)psram((size_t)st.st_size);
    FILE    *f   = buf ? fopen(path, "rb") : NULL;
    size_t   n   = f ? fread(buf, 1, (size_t)st.st_size, f) : 0;
    if (f) {
        fclose(f);
    }
    if (n != (size_t)st.st_size) {
        unalloc(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

typedef struct {
    mesh_ctab_card_t  plan;
    uint8_t          *buf; // MESH_CTAB_CARD_USE: the file's good records
    mesh_cfile_info_t info;
} probe_t;

// A whole side file that is the list (a cut between the remove and the rename) takes the name now, so a save never
// writes over the only whole copy. Inside the card batch.
static void promote_side(void) {
    if (unlink(BIN_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "contacts.bin not removed (errno %d): contacts.new keeps its name", errno);
    } else if (rename(SIDE_PATH, BIN_PATH) != 0) {
        ESP_LOGW(TAG, "contacts.new not renamed (errno %d)", errno);
    }
}

static bool move_aside(char const *path);

// Reads and judges the card's contact files; false when the card's lock was not free (nothing known)
static bool probe(uint32_t wait_ms, probe_t *p) {
    static char const *const PATHS[2] = {BIN_PATH, SIDE_PATH};
    mesh_ctab_file_t         f[2]     = {};
    mesh_cfile_info_t        info[2]  = {};
    uint8_t                 *buf[2]   = {NULL, NULL};
    if (!lichen_fs_sd_begin(meshcore_ctx(), wait_ms)) {
        return false;
    }
    for (int i = 0; i < 2; i++) {
        size_t len    = 0;
        buf[i]        = read_file(PATHS[i], &len, &f[i].exists);
        f[i].verdict  = buf[i] ? mesh_cfile_open(root, buf[i], len, &info[i]) : MESH_CFILE_UNREADABLE;
        f[i].generation = info[i].generation;
        f[i].complete   = info[i].complete;
    }
    bool side = false;
    p->plan   = mesh_ctab_card_plan(true, have_key, &f[0], &f[1], &side);
    p->buf    = NULL;
    if (p->plan == MESH_CTAB_CARD_USE) {
        p->buf         = buf[side ? 1 : 0];
        p->info        = info[side ? 1 : 0];
        buf[side ? 1 : 0] = NULL;
        if (side && p->info.complete) {
            promote_side();
        } else if (side) {
            // One that lost blocks is the best copy there is, and the next save writes contacts.new: it goes aside
            // first, so a cut during that save still leaves it; a damaged contacts.bin beside it is kept too
            move_aside(SIDE_PATH);
            keep_old = f[0].exists;
        }
    }
    lichen_fs_sd_end(meshcore_ctx(), 0);
    unalloc(buf[0]);
    unalloc(buf[1]);
    ESP_LOGI(TAG, "card: contacts.bin %s (verdict %d), contacts.new %s (verdict %d): plan %d", f[0].exists ? "there" : "none",
             (int)f[0].verdict, f[1].exists ? "there" : "none", (int)f[1].verdict, (int)p->plan);
    return true;
}

// What the probe found becomes the next steps: a merge (in parts, under the lock), a fresh file, or hands off
static void start_card(probe_t const *p) {
    switch (p->plan) {
    case MESH_CTAB_CARD_USE:
        merge               = p->buf;
        merge_stat          = {};
        merge_stat.complete = p->info.complete;
        card_failed         = false;
        merge_n             = p->info.good;
        merge_at            = 0;
        generation          = p->info.generation;
        merge_full_before   = merged_full;
        __atomic_store_n(&card_bad_blocks, p->info.bad_blocks, __ATOMIC_RELAXED);
        unalloc(seen_bits);
        seen_bits   = (uint8_t *)psram(SEEN_BITS / 8); // NULL: every record is checked by the exact scan
        if (seen_bits) {
            memset(seen_bits, 0, SEEN_BITS / 8);
        }
        merge_fresh = merge_n == 0; // an empty file: the same path as none at all, but the file is this badge's
        if (p->info.bad_blocks) {
            ESP_LOGW(TAG, "card file: %lu of %lu blocks failed their check, %lu of %lu contacts read",
                     (unsigned long)p->info.bad_blocks, (unsigned long)p->info.blocks, (unsigned long)p->info.good,
                     (unsigned long)p->info.records);
        }
        if (merge_fresh) {
            unalloc(merge);
            merge = NULL;
        }
        break;
    case MESH_CTAB_CARD_FRESH:
        merge_stat  = {};
        card_failed = false;
        generation  = 0;
        merge_fresh = true;
        break;
    case MESH_CTAB_CARD_REFUSED:
        set_place(MESH_CTAB_CARD_LEFT_ALONE);
        notice_pending = true;
        ESP_LOGW(TAG, "the card holds a contact file of another identity or an unreadable one: left alone");
        break;
    case MESH_CTAB_CARD_NONE: break;
    }
}

// ---- merging the card's records into the table (with the mesh lock, or before the task) ------------------------------

static void contact_from_rec(ContactInfo &c, uint8_t const *rec) {
    static_assert(sizeof(c.out_path) >= 64 && sizeof(c.name) == MESH_CTAB_NAME, "contacts3's record layout");
    memset(&c, 0, sizeof(c));
    c.id = mesh::Identity(rec);
    memcpy(c.name, rec + MESH_CTAB_AT_NAME, MESH_CTAB_NAME);
    c.name[MESH_CTAB_NAME - 1] = 0;
    c.type                     = rec[MESH_CTAB_AT_TYPE];
    c.flags                    = rec[MESH_CTAB_AT_FLAGS];
    memcpy(&c.sync_since, rec + 67, 4); // the offsets DataStore::saveContacts writes
    c.out_path_len = rec[71];
    memcpy(&c.last_advert_timestamp, rec + 72, 4);
    memcpy(c.out_path, rec + 76, 64);
    memcpy(&c.lastmod, rec + MESH_CTAB_AT_LASTMOD, 4);
    memcpy(&c.gps_lat, rec + 144, 4);
    memcpy(&c.gps_lon, rec + 148, 4);
}

static void contact_to_rec(uint8_t *rec, ContactInfo const &c) {
    memset(rec, 0, MESH_CTAB_REC); // byte 66 is the unused one DataStore writes as 0
    memcpy(rec, c.id.pub_key, MESH_CTAB_PUB);
    memcpy(rec + MESH_CTAB_AT_NAME, c.name, MESH_CTAB_NAME);
    rec[MESH_CTAB_AT_TYPE]  = c.type;
    rec[MESH_CTAB_AT_FLAGS] = c.flags;
    memcpy(rec + 67, &c.sync_since, 4);
    rec[71] = c.out_path_len;
    memcpy(rec + 72, &c.last_advert_timestamp, 4);
    memcpy(rec + 76, c.out_path, 64);
    memcpy(rec + MESH_CTAB_AT_LASTMOD, &c.lastmod, 4);
    memcpy(rec + 144, &c.gps_lat, 4);
    memcpy(rec + 148, &c.gps_lon, 4);
}

// Whether an earlier record of the card has this key. Only a key the table has can matter (a repeat of an added one
// finds it there). The hash filter keeps the exact scan, O(n) per record, to the rare record whose bit is already set.
static bool card_key_repeats(uint8_t const *rec, bool in_table) {
    if (seen_bits) {
        unsigned h    = (unsigned)(rec[0] | (rec[1] << 8));
        bool     seen = seen_bits[h >> 3] & (1u << (h & 7));
        seen_bits[h >> 3] |= (uint8_t)(1u << (h & 7));
        if (!seen) {
            return false;
        }
    }
    for (size_t i = 0; in_table && i < merge_at; i++) {
        if (memcmp(merge + i * MESH_CTAB_REC, rec, MESH_CTAB_PUB) == 0) {
            return true;
        }
    }
    return false;
}

static void merge_one(uint8_t const *rec) {
    ContactInfo *c = nd->lookupContactByPubKey(rec, PUB_KEY_SIZE);
    uint8_t      table_rec[MESH_CTAB_REC] = {};
    if (c) {
        contact_to_rec(table_rec, *c);
    }
    bool none = rec[MESH_CTAB_AT_TYPE] == ADV_TYPE_NONE;
    mesh_ctab_merge_t plan =
        mesh_ctab_merge_plan(mesh_ctab_del_has(&del, rec), c != NULL, c ? c->lastmod : 0, mesh_ctab_lastmod(rec));
    bool dup = card_key_repeats(rec, c != NULL);
    mesh_ctab_account(&merge_stat, plan, none, dup, c ? table_rec : NULL, rec);
    if (none) {
        return;
    }
    ContactInfo from;
    switch (plan) {
    case MESH_CTAB_ADD:
        contact_from_rec(from, rec);
        // Never through MeshCore's "overwrite the oldest" for a full table: a merge adds, it does not replace
        if (nd->getNumContacts() < MESH_CTAB_CAP_CARD && nd->addContact(from)) {
            merged_added = merged_added + 1;
        } else {
            merged_full = merged_full + 1;
            mesh_ctab_account_full(&merge_stat);
        }
        break;
    case MESH_CTAB_REPLACE:
        contact_from_rec(*c, rec); // shared_secret_valid is false again: the key is the same, the secret is recomputed
        merged_newer = merged_newer + 1;
        break;
    case MESH_CTAB_SKIP: break;
    }
}

// Up to max records; when the file is through, the card holds the list
static void merge_step(size_t max) {
    __atomic_store_n(&cap_now, MESH_CTAB_CAP_CARD, __ATOMIC_RELEASE); // the adds need the room; the place follows
    size_t end = merge_n - merge_at > max ? merge_at + max : merge_n;
    for (; merge_at < end; merge_at++) {
        merge_one(merge + merge_at * MESH_CTAB_REC);
    }
    if (merge_at < merge_n) {
        return;
    }
    bool fresh = merge_fresh && merge_n == 0;
    ESP_LOGI(TAG, "the card holds the contact list now: %lu contacts read from it (%lu new here, %lu newer, %lu without "
                  "room), %d in the table",
             (unsigned long)merge_n, (unsigned long)merged_added, (unsigned long)merged_newer,
             (unsigned long)merged_full, nd->getNumContacts());
    if (!fresh && merged_full != merge_full_before) {
        keep_old = true;
        ESP_LOGW(TAG, "the table is full: %lu contacts of the card's file did not fit; the file is kept as contacts.old",
                 (unsigned long)(merged_full - merge_full_before));
    }
    // The table now also holds what only /storage had: /storage keeps everything it may until the card holds this
    // table. A card file that already is the table is not written again (#472); otherwise the save is asked for at
    // once (MESH_ADVTAB_RETRY_S), not at the interval
    merge_stat.fresh = fresh;
    mesh_ctab_account_done(&merge_stat, (size_t)nd->getNumContacts());
    bool need = mesh_ctab_card_write_needed(&merge_stat);
    drop_merge();
    card_current    = !need;
    card_force      = need;
    card_dirty      = need;
    want_save       = want_save || !need; // a save dropped by the hold is made up; /storage settles on the core set
    last_card_try_s = mesh_uptime_s();
    set_place(MESH_CTAB_ON_CARD);
}

// ---- the deleted keys' file ------------------------------------------------------------------------------------------

static void del_path(char *out, char const *name) {
    snprintf(out, PATH_MAX_LEN, "%s/%s", dir, name);
}

static void del_load(void) {
    char path[PATH_MAX_LEN];
    del_path(path, DEL_NAME);
    FILE  *f = fopen(path, "rb");
    size_t n = f ? fread(del_file, 1, sizeof(del_file), f) : 0;
    if (f) {
        fclose(f);
    }
    if (!mesh_ctab_del_unpack(&del, del_file, n, mesh_uptime_s()) && n) {
        ESP_LOGW(TAG, "%s is damaged: no deleted contacts remembered", path);
    }
    del_saved = del.changes;
}

// With the lock: the list as it is now, for del_write
static void del_pack_locked(void) {
    if (del.changes != del_saved) {
        del_file_len      = mesh_ctab_del_pack(&del, del_file, sizeof(del_file));
        del_saved         = del.changes;
        del_write_pending = true;
    }
}

// LittleFS renames over the old file in one step
static void del_write(void) {
    char path[PATH_MAX_LEN], side[PATH_MAX_LEN];
    del_path(path, DEL_NAME);
    del_path(side, DEL_SIDE_NAME);
    del_write_pending = false;
    if (del_file_len <= 12) { // no keys
        unlink(path);
        return;
    }
    FILE *f  = fopen(side, "wb");
    bool  ok = f && fwrite(del_file, 1, del_file_len, f) == del_file_len;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    if (!ok || rename(side, path) != 0) {
        ESP_LOGE(TAG, "%s not written (errno %d)", path, errno);
        unlink(side);
    }
}

// At boot: a contact deleted just before a crash is still in contacts3
static void del_apply(void) {
    int gone = 0;
    for (size_t i = 0; i < del.n; i++) {
        ContactInfo *c = nd->lookupContactByPubKey(del.key[i], MESH_CTAB_DEL_KEY);
        if (c && nd->removeContact(*c)) {
            gone++;
        }
    }
    if (gone) {
        ESP_LOGI(TAG, "%d deleted contact(s) were still in the saved list: removed again", gone);
        mesh_ctab_run_owner_change();
    }
}

// ---- boot --------------------------------------------------------------------------------------------------------------

void mesh_ctab_run_boot(BadgeMesh *n, char const *files_dir, uint8_t const *prv, size_t prv_len) {
    nd = n;
    snprintf(dir, sizeof(dir), "%s", files_dir);
    last_internal_s = last_card_try_s = mesh_uptime_s();
    keep            = (uint8_t *)psram(MESH_CTAB_CAP_CARD);
    order           = (uint16_t *)psram(MESH_CTAB_CAP_CARD * sizeof(uint16_t));
    convs           = (mesh_conv_t *)psram(MESH_CONV_MAX * sizeof(mesh_conv_t));
    chunk           = (uint8_t *)psram(CHUNK);
    del_load();
    del_apply();
    // Without the scratch memory the card is not used and /storage is written whole, as before #472
    have_key = prv && keep && order && convs && chunk && mesh_cfile_root(root, prv, prv_len);
    probe_t p;
    if (have_key && lichen_fs_sd_mounted() && probe(BOOT_WAIT_MS, &p)) {
        mounted_seen = true;
        start_card(&p);
        while (merge || merge_fresh) {
            merge_step(merge_n);
        }
    }
}

// ---- the saves -----------------------------------------------------------------------------------------------------------

static bool key_in_table(uint8_t const *key, void *ctx) {
    (void)ctx;
    return nd->lookupContactByPubKey(key, MESH_CTAB_DEL_KEY) != nullptr;
}

void mesh_ctab_run_tick_locked(void) {
    if (wipe || !nd) {
        return;
    }
    if (settle_pending) { // the card took the copy of the last review
        settle_pending = false;
        mesh_ctab_del_settle(&del);
    }
    del_pack_locked();
    if (merge || merge_fresh) {
        merge_step(MERGE_PER_LOOP);
        return;
    }
    uint32_t now = mesh_uptime_s();
    bool     due = false;
    if (mesh_ctab_run_place() == MESH_CTAB_ON_CARD && !(retry_at_s && now < retry_at_s)) {
        mesh_advtab_clock_t c = {};
        c.changed             = card_dirty;
        c.on_card             = true;
        c.forced              = card_force;
        c.now_s               = now;
        c.last_try_s          = last_card_try_s;
        due                   = mesh_advtab_due(&c);
    }
    if (deferred && mesh_ctab_flush_due(now, last_internal_s) && !fs_shim_wiped() && fs_shim_release_deferred()) {
        deferred = false; // the interval is up: this loop's take picks the parked save up and the write follows (#429)
    }
    if ((due || want_save) && !fs_shim_wiped()) {
        card_take = due;
        want_save = false;
        store.saveContacts(nd, keep_contact); // parked by the hold; the loop takes it before its unlock
    }
}

void mesh_ctab_run_taken_locked(void) {
    convs_n = convs ? mesh_store_convs(convs, MESH_CONV_MAX) : 0;
    mesh_ctab_del_review(&del, mesh_uptime_s(), key_in_table, NULL);
    if (mesh_ctab_run_place() == MESH_CTAB_ON_CARD) {
        card_dirty = true; // MeshCore saved: something in the table changed
    }
}

// Always on /storage: a contact with a conversation, or a repeater or room whose password is remembered (#288)
static bool always_in(uint8_t const *rec, void *ctx) {
    (void)ctx;
    for (size_t i = 0; i < convs_n; i++) {
        if (convs[i].key.kind == MESH_CHAT_DM && memcmp(convs[i].key.id, rec, sizeof(convs[i].key.id)) == 0) {
            return true;
        }
    }
    uint8_t type = rec[MESH_CTAB_AT_TYPE];
    if (type != ADV_TYPE_REPEATER && type != ADV_TYPE_ROOM) {
        return false;
    }
    char pw[PW_BUF];
    bool has = meshcore_pw_get(rec, pw, sizeof(pw));
    mh_wipe(pw, sizeof(pw));
    return has;
}

// The side file read back against what was written
static bool verify(uint8_t const *want, size_t len, int *err) {
    FILE *f = fopen(SIDE_PATH, "rb");
    if (!f) {
        *err = errno;
        return false;
    }
    size_t at = 0;
    bool   ok = true;
    while (ok && at < len) {
        size_t part = len - at < CHUNK ? len - at : CHUNK;
        ok          = fread(chunk, 1, part, f) == part && memcmp(chunk, want + at, part) == 0;
        at += part;
    }
    ok = ok && fgetc(f) == EOF;
    fclose(f);
    if (!ok) {
        *err = EIO; // read back differently: the card, not a file call, failed
    }
    return ok;
}

// contacts.bin makes room for the new file: removed, or kept as contacts.old when the merge left some of its contacts
// out of a full table or was damaged (with both old names taken the save fails and the file stays)
static bool retire_bin(void) {
    int slot = keep_old ? mesh_ctab_aside_slot(exists(OLD_PATH), exists(OLD1_PATH)) : -1;
    if (keep_old && slot < 0 && exists(BIN_PATH)) { // never deleted: the save fails until the owner makes room
        ESP_LOGW(TAG, "contacts.old and contacts.old.1 both exist: the card's earlier list has nowhere to go");
        errno = EEXIST;
        return false;
    }
    if (slot >= 0 && exists(BIN_PATH)) {
        return rename(BIN_PATH, slot == 0 ? OLD_PATH : OLD1_PATH) == 0;
    }
    return unlink(BIN_PATH) == 0 || errno == ENOENT;
}

// The file as contacts.bin through contacts.new, checked before it takes the name. FAT cannot rename over a file: the
// old one is removed first, and a cut in between leaves contacts.new, whole, which the next probe gives its name.
static bool card_file_write(uint8_t const *file, size_t len, int *err) {
    bool ok = (mkdir(SD_ROOT, 0755) == 0 || errno == EEXIST) && (mkdir(SD_DIR, 0755) == 0 || errno == EEXIST);
    FILE *f = ok ? fopen(SIDE_PATH, "wb") : NULL;
    ok      = f && fwrite(file, 1, len, f) == len;
    *err    = ok ? 0 : errno;
    if (f && fclose(f) != 0 && ok) {
        ok   = false;
        *err = errno;
    }
    ok = ok && verify(file, len, err);
    if (ok && !retire_bin()) {
        ok   = false;
        *err = errno;
    }
    if (ok && rename(SIDE_PATH, BIN_PATH) != 0) {
        ok   = false;
        *err = errno;
    }
    if (!ok) {
        unlink(SIDE_PATH);
        ESP_LOGE(TAG, "contact list not written to the card (errno %d): the file there stays as it was", *err);
    }
    return ok;
}

// The whole list to the card. A failure leaves the card's file as it was and makes /storage keep everything again
// until a save works; a busy card is tried again RETRY_S later.
static void write_card(uint8_t const *recs, size_t n, uint32_t wait_ms) {
    // Whatever keeps this save from the card, /storage must not count on the card for it
    card_current = false;
    if (merge || merge_fresh || !have_key || !lichen_fs_sd_mounted()) {
        return; // a file not merged yet must not be replaced by a table without its contacts
    }
    int64_t  started = esp_timer_get_time();
    size_t   size    = mesh_cfile_size(n);
    uint8_t *file    = (uint8_t *)psram(size);
    uint8_t  salt[MESH_CFILE_SALT];
    lichen_random_fill(salt, sizeof(salt));
    size_t len = file ? mesh_cfile_pack(root, salt, generation + 1, recs, n, file, size) : 0;
    if (len && !lichen_fs_sd_begin(meshcore_ctx(), wait_ms)) {
        retry_at_s = mesh_uptime_s() + RETRY_S;
        unalloc(file);
        if (stopping) { // no later try before the restart
            ESP_LOGE(TAG, "the card was busy at the stop: the contact list there is not the newest");
        }
        return;
    }
    int  err = 0;
    bool ok  = len && card_file_write(file, len, &err);
    if (len) {
        lichen_fs_sd_end(meshcore_ctx(), err);
    }
    unalloc(file);
    last_card_try_s = mesh_uptime_s();
    card_current    = ok;
    if (!ok) {
        card_errors = card_errors + 1;
        card_failed = true;
        card_force  = false; // the next try at the interval, not every minute
        return;
    }
    generation++;
    card_failed     = false;
    keep_old        = false;
    card_dirty      = false;
    card_force      = false;
    settle_pending  = true;
    card_last_bytes = (uint32_t)len;
    card_last_ms    = (uint32_t)((esp_timer_get_time() - started) / 1000);
    card_writes     = card_writes + 1;
    ESP_LOGI(TAG, "contact list on the card: %u contacts, %u B, %lu ms", (unsigned)n, (unsigned)len,
             (unsigned long)card_last_ms);
}

// #429: the save against contacts3 on /storage (mesh_ctab_file_diff reads it in chunks of whole records into the
// scratch buffer: no new memory, one pass of a few ms). Anything unreadable counts as changed.
typedef struct {
    fs_shim_held_t const *h;
    FILE                 *f;
} diff_ctx_t;

static size_t diff_read(void *ctx, size_t at, uint8_t *out, size_t want) {
    FILE *f = ((diff_ctx_t *)ctx)->f;
    return fseek(f, (long)at, SEEK_SET) == 0 ? fread(out, 1, want, f) : 0;
}

static mesh_ctab_diff_t file_diff(void *ctx) {
    fs_shim_held_t const *h = ((diff_ctx_t *)ctx)->h;
    char                  path[PATH_MAX_LEN];
    struct stat           st;
    if (!h->path || snprintf(path, sizeof(path), "%s%s", dir, h->path) >= (int)sizeof(path) || stat(path, &st) != 0) {
        return MESH_CTAB_CHANGED;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return MESH_CTAB_CHANGED;
    }
    ((diff_ctx_t *)ctx)->f = f;
    mesh_ctab_diff_t d     = mesh_ctab_file_diff(h->buf, h->len, true, (size_t)st.st_size, diff_read, ctx, chunk, CHUNK);
    fclose(f);
    return d;
}

// #429: a save that only moved timestamps goes back into the shim's slot (MeshCore and the screens read it again) and
// is written when the interval is up. While the file is compared the slot is empty and readers see the file on disk
// (as after any take); a save that opens meanwhile makes a second buffer for those few tens of ms. The slot is the mesh lock's: taken for this one call, no file work under it.
static bool park_again(fs_shim_held_t *h, bool forced, uint32_t now) {
    diff_ctx_t c = {h, NULL};
    if (!chunk || !mesh_ctab_park(forced, now, last_internal_s, file_diff, &c)) {
        return false;
    }
    mesh_lock();
    bool wiped = fs_shim_wiped(); // a factory reset since the take: the pre-reset list is not parked behind it
    if (wiped) {
        fs_shim_free_held(h);
    } else {
        fs_shim_defer_held(h);
    }
    mesh_unlock();
    deferred = !wiped;
    if (!wiped) {
        internal_parked = internal_parked + 1;
    }
    return true;
}

static bool internal_held(void) {
    // From a mounted card not yet looked at until its list is merged, /storage keeps what it has (not at a stop, which
    // writes what the table holds). The save after the merge writes it; owner_dirty stays set for that one. A card
    // that mounts after this save cannot be held.
    mesh_ctab_hold_t hold = {};
    hold.merging          = merge || merge_fresh;
    hold.card_mounted     = lichen_fs_sd_mounted();
    hold.card_pending     = hold.card_mounted && !mounted_seen && have_key && !wipe;
    // Armed here and not in the loop's work step, which runs after the save: a save in the loop the card appears in
    // is held too
    uint32_t now          = mesh_uptime_s();
    if (!hold.card_pending) {
        pending_since_s = 0;
    } else if (!pending_since_s) {
        pending_since_s = now ? now : 1; // 0 means not pending
    }
    hold.pending_age_s    = pending_since_s ? now - pending_since_s : 0;
    hold.card_current     = card_current;
    hold.card_failed      = card_failed;
    return mesh_ctab_hold_internal(&hold);
}

// contacts3 on /storage. While the card holds the list: the core set, and only when the owner changed something, at
// the interval or at a stop. Otherwise everything (the newest MESH_CTAB_CAP_INTERNAL of a larger table), every time.
static void write_internal(fs_shim_held_t *h, size_t n, int contacts) {
    if (!stopping && internal_held()) {
        save_held = true;
        fs_shim_free_held(h);
        internal_skips = internal_skips + 1;
        return;
    }
    bool     core   = mesh_ctab_run_place() == MESH_CTAB_ON_CARD && card_current;
    size_t   limit  = core ? MESH_CTAB_CORE : MESH_CTAB_CAP_INTERNAL;
    bool     filter = keep && order && n <= MESH_CTAB_CAP_CARD && (core || n > limit);
    size_t   kept   = n;
    uint32_t digest = 0, now = mesh_uptime_s();
    bool     owner  = __atomic_exchange_n(&owner_dirty, false, __ATOMIC_ACQ_REL);
    deferred        = false; // this save is the newest: nothing older stays parked behind it
    if (filter) {
        kept   = mesh_ctab_select(h->buf, n, limit, always_in, NULL, keep, order);
        digest = mesh_ctab_digest(h->buf, n, keep);
    }
    bool changed = owner || !have_digest || digest != last_digest;
    if (core && filter && !stopping && !changed && now - last_internal_s < MESH_ADVTAB_INTERNAL_S) {
        fs_shim_free_held(h);
        internal_skips = internal_skips + 1;
        return;
    }
    if (filter) {
        h->len = mesh_ctab_compact(h->buf, n, keep) * MESH_CTAB_REC;
    }
    if (park_again(h, stopping || owner, now)) {
        return;
    }
    fs_shim_wrote_t w = File::writeHeld(h);
    ESP_LOGI(TAG, "contacts saved after the unlock: %d contacts, %u B, %u B rewritten, %lu ms%s (%u on /storage)",
             contacts, (unsigned)w.len, (unsigned)w.rewritten, (unsigned long)(w.us / 1000), w.ok ? "" : " (failed)",
             (unsigned)kept);
    if (!w.ok) {
        if (owner) {
            mesh_ctab_run_owner_change(); // not lost: the next save writes
        }
        return;
    }
    last_digest     = digest;
    have_digest     = filter;
    last_internal_s = now;
    internal_kept   = (uint32_t)kept;
    internal_writes = internal_writes + 1;
}

void mesh_ctab_run_saved(fs_shim_held_t *h, int contacts) {
    if (!h->set) {
        return;
    }
    size_t n    = h->len / MESH_CTAB_REC;
    bool   card = card_take || (stopping && mesh_ctab_run_place() == MESH_CTAB_ON_CARD && card_dirty);
    card_take   = false;
    if (card) {
        write_card(h->buf, n, stopping ? STOP_WAIT_MS : WRITE_WAIT_MS);
    }
    write_internal(h, n, contacts);
}

// The file's head carries this identity's check (its blocks are not looked at)
static bool head_is_ours(char const *path) {
    uint8_t           head[MESH_CFILE_HEAD];
    mesh_cfile_info_t info;
    FILE             *f = fopen(path, "rb");
    size_t            n = f ? fread(head, 1, sizeof(head), f) : 0;
    if (f) {
        fclose(f);
    }
    mesh_cfile_verdict_t v = mesh_cfile_open(root, head, n, &info);
    return v == MESH_CFILE_OK || v == MESH_CFILE_DAMAGED;
}

// After a factory reset: this badge's card file goes too (the new identity could not read it anyway). A busy card
// keeps it: an accepted gap, logged; the new identity then finds a file it leaves alone.
static void remove_card_file(void) {
    bool ours = mesh_ctab_run_place() == MESH_CTAB_ON_CARD || merge || merge_fresh;
    drop_merge();
    if (!ours || !lichen_fs_sd_mounted()) {
        return;
    }
    if (!lichen_fs_sd_begin(meshcore_ctx(), STOP_WAIT_MS)) {
        ESP_LOGE(TAG, "the card was busy: its contact file was not removed");
        return;
    }
    for (int i = 0; i < 2; i++) { // a card swapped since the last look: another badge's file stays
        char const *path = i == 0 ? BIN_PATH : SIDE_PATH;
        if (head_is_ours(path)) {
            unlink(path);
        } else if (exists(path)) {
            ESP_LOGW(TAG, "%s is not this badge's: not removed", path);
        }
    }
    lichen_fs_sd_end(meshcore_ctx(), 0);
}

void mesh_ctab_run_stop_locked(bool final) {
    if (!nd) {
        return;
    }
    if (wipe) {
        remove_card_file();
        set_place(MESH_CTAB_ON_STORAGE);
    } else if (!fs_shim_wiped()) {
        fs_shim_held_t h;
        stopping = true;
        fs_shim_drop_held();
        store.saveContacts(nd, keep_contact); // the hold is still on: parked, then written here by the rules above
        fs_shim_take_held(&h);
        mesh_ctab_run_taken_locked();
        mesh_ctab_run_saved(&h, nd->getNumContacts());
        stopping = false;
        if (settle_pending) {
            settle_pending = false;
            mesh_ctab_del_settle(&del);
        }
        del_pack_locked();
        if (del_write_pending) {
            del_write();
        }
    }
    if (final) {
        mh_wipe(root, sizeof(root));
        have_key = false;
    }
}

void mesh_ctab_run_wiped(void) {
    wipe = true;
    memset(&del, 0, sizeof(del));
    del_saved         = 0;
    del_write_pending = false; // the format took the file
}

// ---- the card coming and going, the owner's request (the mesh task, after the unlock) ---------------------------------

static void card_gone(void) {
    mounted_seen = false;
    drop_merge();
    if (mesh_ctab_run_place() == MESH_CTAB_ON_CARD || mesh_ctab_run_cap() != MESH_CTAB_CAP_INTERNAL) {
        // Nothing leaves the table; /storage takes what it may hold at the next loop's save
        ESP_LOGW(TAG, "the card is gone: the contact list is saved on /storage again");
        want_save = true;
    }
    card_current = card_dirty = false;
    set_place(MESH_CTAB_ON_STORAGE);
}

static void card_came(void) {
    probe_t  p;
    uint32_t now = mesh_uptime_s();
    if (retry_at_s && now < retry_at_s) {
        return;
    }
    retry_at_s = 0;
    if (!probe(WRITE_WAIT_MS, &p)) {
        retry_at_s = now + RETRY_S;
        return;
    }
    mounted_seen = true;
    start_card(&p);
}

// Moves a file aside under a free contacts.old name; true also when there is no such file
static bool move_aside(char const *path) {
    if (!exists(path)) {
        return true;
    }
    int slot = mesh_ctab_aside_slot(exists(OLD_PATH), exists(OLD1_PATH));
    return slot >= 0 && rename(path, slot == 0 ? OLD_PATH : OLD1_PATH) == 0;
}

static void use_card(void) {
    if (mesh_ctab_run_place() != MESH_CTAB_CARD_LEFT_ALONE) {
        return; // nothing was left alone: the row follows the place
    }
    char const *why = NULL;
    if (!lichen_fs_sd_mounted()) {
        why = "No card";
    } else if (!lichen_fs_sd_begin(meshcore_ctx(), STOP_WAIT_MS)) {
        why = "Card busy: try again";
    } else {
        int need = (exists(BIN_PATH) ? 1 : 0) + (exists(SIDE_PATH) ? 1 : 0);
        int room = (exists(OLD_PATH) ? 0 : 1) + (exists(OLD1_PATH) ? 0 : 1);
        if (need > room || !move_aside(BIN_PATH) || !move_aside(SIDE_PATH)) { // all or nothing
            why = "Remove contacts.old first"; // both aside names are taken; neither is ever overwritten
            ESP_LOGW(TAG, "contacts.old and contacts.old.1 both exist on the card: nothing moved");
        }
        lichen_fs_sd_end(meshcore_ctx(), 0);
    }
    __atomic_store_n(&use_error, why, __ATOMIC_RELEASE);
    if (!why) {
        ESP_LOGI(TAG, "the owner gave this card to the contact list: the file there was moved aside");
        set_place(MESH_CTAB_ON_STORAGE);
        generation  = 0;
        merge_stat  = {};
        merge_fresh = true; // the next loop makes the card the list's place and writes it
    }
}

void mesh_ctab_run_work(void) {
    bool mounted = lichen_fs_sd_mounted();
    if (mesh_ctab_save_owed(save_held, internal_held())) { // refused, no file, failed, pulled or timed out: the save is still owed
        save_held = false;
        want_save = true;
    }
    if (!mounted && mounted_seen) {
        card_gone();
    } else if (mounted && !mounted_seen && have_key && !wipe) {
        card_came();
    }
    if (__atomic_exchange_n(&use_request, false, __ATOMIC_ACQ_REL)) {
        use_card();
    }
    if (del_write_pending) {
        del_write();
    }
    if (notice_pending) {
        notice_pending = false;
        lichen_notify_to(meshcore_ctx(), LN_WARN, "contacts-card", "Card not used for contacts",
                         "This card holds another badge's contact list, or one that cannot be read. It is left as it "
                         "is. To use the card: MeshCore settings, Contacts on the card.",
                         LN_COALESCE);
    }
}

void mesh_ctab_run_console(void) {
    static char const *const PLACE[] = {"on /storage", "on the card", "on /storage (the card's file is left alone)"};
    console_reply("contacts: list %s, room for %d | card file %lu, last %lu bytes in %lu ms, %lu written, %lu failed, "
                  "%lu bad blocks read | /storage %lu kept, %lu written, %lu held back, %lu parked | merged %lu new, %lu newer, "
                  "%lu without room",
                  PLACE[mesh_ctab_run_place()], mesh_ctab_run_cap(), (unsigned long)generation,
                  (unsigned long)card_last_bytes, (unsigned long)card_last_ms, (unsigned long)card_writes,
                  (unsigned long)card_errors, (unsigned long)card_bad_blocks, (unsigned long)internal_kept,
                  (unsigned long)internal_writes, (unsigned long)internal_skips, (unsigned long)internal_parked,
                  (unsigned long)merged_added,
                  (unsigned long)merged_newer, (unsigned long)merged_full);
}
