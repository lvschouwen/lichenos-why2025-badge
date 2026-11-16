#include "mesh_ctab.h"

#include "mesh_hist_crypto.h"

#include <string.h>

static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t get16(uint8_t const *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

// FNV-1a: a change detector and a file checksum against torn writes, not a defence
static uint32_t fnv(uint32_t h, uint8_t const *p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}
#define FNV_START 2166136261u

uint32_t mesh_ctab_lastmod(uint8_t const *rec) {
    return get32(rec + MESH_CTAB_AT_LASTMOD);
}

// ---- timestamp-only saves stay parked (#429) ------------------------------------------------------------------------

mesh_ctab_diff_t mesh_ctab_diff(uint8_t const *a, uint8_t const *b, size_t n) {
    mesh_ctab_diff_t worst = MESH_CTAB_SAME;
    for (size_t i = 0; i < n; i++, a += MESH_CTAB_REC, b += MESH_CTAB_REC) {
        if (memcmp(a, b, MESH_CTAB_REC) == 0) {
            continue;
        }
        // Everything but the two timestamps must match: compare the spans around them
        bool rest = memcmp(a, b, MESH_CTAB_AT_ADVERT) == 0 &&
                    memcmp(a + MESH_CTAB_AT_ADVERT + 4, b + MESH_CTAB_AT_ADVERT + 4,
                           MESH_CTAB_AT_LASTMOD - MESH_CTAB_AT_ADVERT - 4) == 0 &&
                    memcmp(a + MESH_CTAB_AT_LASTMOD + 4, b + MESH_CTAB_AT_LASTMOD + 4,
                           MESH_CTAB_REC - MESH_CTAB_AT_LASTMOD - 4) == 0;
        if (!rest) {
            return MESH_CTAB_CHANGED;
        }
        worst = MESH_CTAB_TIMES_ONLY;
    }
    return worst;
}

bool mesh_ctab_flush_due(uint32_t now_s, uint32_t last_write_s) {
    return now_s - last_write_s >= MESH_CTAB_SAVE_INTERVAL_S; // unsigned: a clock that went back writes, never waits
}

bool mesh_ctab_defer(mesh_ctab_diff_t d, bool forced, uint32_t now_s, uint32_t last_write_s) {
    return d == MESH_CTAB_TIMES_ONLY && !forced && !mesh_ctab_flush_due(now_s, last_write_s);
}

// ---- which contacts /storage keeps ----------------------------------------------------------------------------------

// The newer record first; the earlier one on a tie
static bool before(uint8_t const *recs, uint16_t a, uint16_t b) {
    uint32_t ma = mesh_ctab_lastmod(recs + (size_t)a * MESH_CTAB_REC);
    uint32_t mb = mesh_ctab_lastmod(recs + (size_t)b * MESH_CTAB_REC);
    return ma != mb ? ma > mb : a < b;
}

static void sort_newest_first(uint8_t const *recs, uint16_t *order, size_t n) {
    for (size_t gap = n / 2; gap > 0; gap /= 2) { // a shell sort: no recursion, no callback state
        for (size_t i = gap; i < n; i++) {
            uint16_t v = order[i];
            size_t   j = i;
            for (; j >= gap && before(recs, v, order[j - gap]); j -= gap) {
                order[j] = order[j - gap];
            }
            order[j] = v;
        }
    }
}

size_t mesh_ctab_select(uint8_t const *recs, size_t n, size_t limit, bool (*always)(uint8_t const *rec, void *ctx),
                        void *ctx, uint8_t *keep, uint16_t *order) {
    size_t kept = 0, others = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t const *rec = recs + i * MESH_CTAB_REC;
        bool           in  = (rec[MESH_CTAB_AT_FLAGS] & MESH_CTAB_FAV) != 0 || (always && always(rec, ctx));
        keep[i]            = in ? MESH_CTAB_ALWAYS : MESH_CTAB_OUT;
        if (in) {
            kept++;
        } else {
            order[others++] = (uint16_t)i;
        }
    }
    if (kept >= limit) {
        return kept;
    }
    if (others > limit - kept) { // only then does the order matter
        sort_newest_first(recs, order, others);
        others = limit - kept;
    }
    for (size_t i = 0; i < others; i++) {
        keep[order[i]] = MESH_CTAB_RECENT;
    }
    return kept + others;
}

size_t mesh_ctab_compact(uint8_t *recs, size_t n, uint8_t const *keep) {
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        if (keep[i] == MESH_CTAB_OUT) {
            continue;
        }
        if (out != i) {
            memmove(recs + out * MESH_CTAB_REC, recs + i * MESH_CTAB_REC, MESH_CTAB_REC);
        }
        out++;
    }
    return out;
}

uint32_t mesh_ctab_digest(uint8_t const *recs, size_t n, uint8_t const *keep) {
    uint32_t h = FNV_START;
    for (size_t i = 0; i < n; i++) {
        if (keep[i] == MESH_CTAB_ALWAYS) { // key, name, type and flags lie together at the record's start
            h = fnv(h, recs + i * MESH_CTAB_REC, MESH_CTAB_AT_FLAGS + 1);
        }
    }
    return h;
}

mesh_ctab_merge_t mesh_ctab_merge_plan(bool deleted, bool in_table, uint32_t table_lastmod, uint32_t card_lastmod) {
    if (!in_table) {
        return deleted ? MESH_CTAB_SKIP : MESH_CTAB_ADD;
    }
    return card_lastmod > table_lastmod ? MESH_CTAB_REPLACE : MESH_CTAB_SKIP;
}

// ---- contacts the owner deleted --------------------------------------------------------------------------------------

static int del_find(mesh_ctab_del_t const *d, uint8_t const *pub) {
    for (size_t i = 0; i < d->n; i++) {
        if (memcmp(d->key[i], pub, MESH_CTAB_DEL_KEY) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void del_drop(mesh_ctab_del_t *d, size_t at) {
    for (size_t i = at; i + 1 < d->n; i++) {
        memcpy(d->key[i], d->key[i + 1], MESH_CTAB_DEL_KEY);
        d->added_s[i] = d->added_s[i + 1];
        d->settled[i] = d->settled[i + 1];
    }
    d->n--;
    d->changes++;
}

void mesh_ctab_del_add(mesh_ctab_del_t *d, uint8_t const *pub, uint32_t now_s) {
    int at = del_find(d, pub);
    if (at < 0) {
        if (d->n == MESH_CTAB_DEL) {
            del_drop(d, 0);
        }
        at = (int)d->n++;
        memcpy(d->key[at], pub, MESH_CTAB_DEL_KEY);
        d->changes++;
    }
    d->added_s[at] = now_s;
    d->settled[at] = false;
}

bool mesh_ctab_del_has(mesh_ctab_del_t const *d, uint8_t const *pub) {
    return del_find(d, pub) >= 0;
}

void mesh_ctab_del_review(mesh_ctab_del_t *d, uint32_t now_s, bool (*in_table)(uint8_t const *key, void *ctx),
                          void *ctx) {
    for (size_t i = d->n; i-- > 0;) {
        bool there    = in_table(d->key[i], ctx);
        d->settled[i] = !there;
        if (there && now_s - d->added_s[i] >= MESH_CTAB_DEL_WAIT) {
            del_drop(d, i);
        }
    }
}

void mesh_ctab_del_settle(mesh_ctab_del_t *d) {
    for (size_t i = d->n; i-- > 0;) {
        if (d->settled[i]) {
            del_drop(d, i);
        }
    }
}

static char const DEL_MAGIC[4] = {'L', 'D', 'E', 'L'};

size_t mesh_ctab_del_pack(mesh_ctab_del_t const *d, uint8_t *buf, size_t cap) {
    size_t body = 8 + d->n * MESH_CTAB_DEL_KEY;
    if (cap < body + 4) {
        return 0;
    }
    memcpy(buf, DEL_MAGIC, 4);
    put16(buf + 4, 1);
    put16(buf + 6, (uint16_t)d->n);
    memcpy(buf + 8, d->key, d->n * MESH_CTAB_DEL_KEY);
    put32(buf + body, fnv(FNV_START, buf, body));
    return body + 4;
}

bool mesh_ctab_del_unpack(mesh_ctab_del_t *d, uint8_t const *buf, size_t len, uint32_t now_s) {
    memset(d, 0, sizeof(*d));
    if (len < 12 || memcmp(buf, DEL_MAGIC, 4) != 0 || get16(buf + 4) != 1) {
        return false;
    }
    size_t n = get16(buf + 6), body = 8 + n * MESH_CTAB_DEL_KEY;
    if (n > MESH_CTAB_DEL || len != body + 4 || get32(buf + body) != fnv(FNV_START, buf, body)) {
        return false;
    }
    memcpy(d->key, buf + 8, n * MESH_CTAB_DEL_KEY);
    for (size_t i = 0; i < n; i++) {
        d->added_s[i] = now_s;
    }
    d->n = n;
    return true;
}

// ---- the card file ---------------------------------------------------------------------------------------------------

#define HEAD_SIGNED     (MESH_CFILE_HEAD - MESH_CFILE_TAG) // 52: everything before the head's tag
#define AT_SALT         20
#define AT_CHECK        36
#define BLOCK_BYTES_MAX (MESH_CFILE_BLOCK * MESH_CTAB_REC)

static char const CON_MAGIC[4] = {'L', 'C', 'O', 'N'};
static char const ROOT_LABEL[] = "lichen-contacts-v1";

typedef struct {
    uint8_t enc[16], mac[32], check[16];
} cfile_keys_t;

bool mesh_cfile_root(uint8_t root[32], uint8_t const *prv, size_t prv_len) {
    return prv && prv_len && mh_sha256(root, ROOT_LABEL, sizeof(ROOT_LABEL) - 1, prv, prv_len);
}

static bool derive(cfile_keys_t *k, uint8_t const root[32], uint8_t const *salt) {
    uint8_t out[32];
    bool    ok = mh_hmac(out, root, 32, "enc", 3, salt, MESH_CFILE_SALT, NULL, 0);
    memcpy(k->enc, out, sizeof(k->enc));
    ok = ok && mh_hmac(k->mac, root, 32, "mac", 3, salt, MESH_CFILE_SALT, NULL, 0);
    ok = ok && mh_hmac(out, root, 32, "check", 5, salt, MESH_CFILE_SALT, NULL, 0);
    memcpy(k->check, out, sizeof(k->check));
    mh_wipe(out, sizeof(out));
    return ok;
}

static bool head_tag(cfile_keys_t const *k, uint8_t const *head, uint8_t tag[MESH_CFILE_TAG]) {
    uint8_t out[32];
    bool    ok = mh_hmac(out, k->mac, sizeof(k->mac), "head", 4, head, HEAD_SIGNED, NULL, 0);
    memcpy(tag, out, MESH_CFILE_TAG);
    return ok;
}

static bool block_tag(cfile_keys_t const *k, uint32_t block, uint8_t const *data, size_t len,
                      uint8_t tag[MESH_CFILE_TAG]) {
    uint8_t out[32], number[4];
    put32(number, block);
    bool ok = mh_hmac(out, k->mac, sizeof(k->mac), "blk", 3, number, sizeof(number), data, len);
    memcpy(tag, out, MESH_CFILE_TAG);
    return ok;
}

static bool block_crypt(cfile_keys_t const *k, uint32_t block, uint8_t *data, size_t len) {
    uint8_t iv[12] = {0};
    put32(iv, block);
    return mh_aes128_ctr(k->enc, iv, data, len);
}

// Not cut short at the first difference
static bool same_tag(uint8_t const *a, uint8_t const *b) {
    uint8_t diff = 0;
    for (size_t i = 0; i < MESH_CFILE_TAG; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static size_t block_count(size_t n) {
    return (n + MESH_CFILE_BLOCK - 1) / MESH_CFILE_BLOCK;
}

// The bytes of block number b's records when the file holds n
static size_t block_bytes(size_t n, size_t b) {
    size_t left = n - b * MESH_CFILE_BLOCK;
    return (left < MESH_CFILE_BLOCK ? left : MESH_CFILE_BLOCK) * MESH_CTAB_REC;
}

size_t mesh_cfile_size(size_t n) {
    return MESH_CFILE_HEAD + n * MESH_CTAB_REC + block_count(n) * MESH_CFILE_TAG;
}

size_t mesh_cfile_pack(uint8_t const root[32], uint8_t const salt[MESH_CFILE_SALT], uint32_t generation,
                       uint8_t const *recs, size_t n, uint8_t *out, size_t cap) {
    cfile_keys_t k;
    size_t       size = mesh_cfile_size(n);
    if (n > MESH_CTAB_CAP_CARD || cap < size) {
        return 0;
    }
    if (!derive(&k, root, salt)) {
        mh_wipe(&k, sizeof(k));
        return 0;
    }
    memcpy(out, CON_MAGIC, 4);
    put16(out + 4, MESH_CFILE_VERSION);
    put16(out + 6, MESH_CTAB_REC);
    put32(out + 8, generation);
    put32(out + 12, (uint32_t)n);
    put32(out + 16, MESH_CFILE_BLOCK);
    memcpy(out + AT_SALT, salt, MESH_CFILE_SALT);
    memcpy(out + AT_CHECK, k.check, sizeof(k.check));
    bool     ok = head_tag(&k, out, out + HEAD_SIGNED);
    uint8_t *at = out + MESH_CFILE_HEAD;
    for (size_t b = 0; ok && b < block_count(n); b++) {
        size_t bytes = block_bytes(n, b);
        memcpy(at, recs + b * BLOCK_BYTES_MAX, bytes);
        ok = block_crypt(&k, (uint32_t)b, at, bytes) && block_tag(&k, (uint32_t)b, at, bytes, at + bytes);
        at += bytes + MESH_CFILE_TAG;
    }
    mh_wipe(&k, sizeof(k));
    return ok ? size : 0;
}

// The head's fixed fields; false when this is no contact file of this format
static bool head_parse(uint8_t const *buf, size_t len, mesh_cfile_info_t *info) {
    if (len < MESH_CFILE_HEAD || memcmp(buf, CON_MAGIC, 4) != 0 || get16(buf + 4) != MESH_CFILE_VERSION ||
        get16(buf + 6) != MESH_CTAB_REC || get32(buf + 16) != MESH_CFILE_BLOCK) {
        return false;
    }
    info->generation = get32(buf + 8);
    info->records    = get32(buf + 12);
    info->blocks     = (uint32_t)block_count(info->records);
    return info->records <= MESH_CTAB_CAP_CARD;
}

// Checks and decrypts the blocks, moving the good ones' records to the front; false when a primitive failed
static bool blocks_open(cfile_keys_t const *k, uint8_t *buf, size_t len, mesh_cfile_info_t *info) {
    size_t at = MESH_CFILE_HEAD;
    for (size_t b = 0; b < info->blocks; b++) {
        size_t  bytes = block_bytes(info->records, b);
        uint8_t tag[MESH_CFILE_TAG];
        if (at + bytes + MESH_CFILE_TAG > len) { // cut off: this one and every block after it
            info->bad_blocks += info->blocks - (uint32_t)b;
            break;
        }
        if (!block_tag(k, (uint32_t)b, buf + at, bytes, tag)) {
            return false;
        }
        if (!same_tag(tag, buf + at + bytes)) {
            info->bad_blocks++;
        } else {
            if (!block_crypt(k, (uint32_t)b, buf + at, bytes)) {
                return false;
            }
            memmove(buf + (size_t)info->good * MESH_CTAB_REC, buf + at, bytes);
            info->good += (uint32_t)(bytes / MESH_CTAB_REC);
        }
        at += bytes + MESH_CFILE_TAG;
    }
    return true;
}

mesh_cfile_verdict_t mesh_cfile_open(uint8_t const root[32], uint8_t *buf, size_t len, mesh_cfile_info_t *info) {
    cfile_keys_t k;
    uint8_t      tag[MESH_CFILE_TAG];
    memset(info, 0, sizeof(*info));
    if (!head_parse(buf, len, info)) {
        return MESH_CFILE_UNREADABLE;
    }
    mesh_cfile_verdict_t v = MESH_CFILE_OK;
    if (!derive(&k, root, buf + AT_SALT) || !head_tag(&k, buf, tag)) {
        v = MESH_CFILE_CRYPTO;
    } else if (!same_tag(k.check, buf + AT_CHECK)) {
        v = MESH_CFILE_FOREIGN;
    } else if (!same_tag(tag, buf + HEAD_SIGNED)) {
        v = MESH_CFILE_DAMAGED;
    } else if (!blocks_open(&k, buf, len, info)) {
        v = MESH_CFILE_CRYPTO;
    } else if (info->bad_blocks * 10 > info->blocks) {
        v = MESH_CFILE_DAMAGED;
    } else {
        info->complete = info->bad_blocks == 0;
    }
    mh_wipe(&k, sizeof(k));
    return v;
}

// ---- what a start, or a card put in, does with the card ---------------------------------------------------------------

static bool is(mesh_ctab_file_t const *f, mesh_cfile_verdict_t v) {
    return f->exists && f->verdict == v;
}

mesh_ctab_card_t mesh_ctab_card_plan(bool card, bool key, mesh_ctab_file_t const *bin, mesh_ctab_file_t const *side,
                                     bool *use_side) {
    *use_side = false;
    if (!card || !key) {
        return MESH_CTAB_CARD_NONE;
    }
    if (is(bin, MESH_CFILE_FOREIGN) || is(side, MESH_CFILE_FOREIGN)) {
        return MESH_CTAB_CARD_REFUSED;
    }
    if (is(bin, MESH_CFILE_CRYPTO) || is(side, MESH_CFILE_CRYPTO)) {
        return MESH_CTAB_CARD_NONE;
    }
    bool bin_ok = is(bin, MESH_CFILE_OK), side_ok = is(side, MESH_CFILE_OK);
    if (bin_ok || side_ok) {
        *use_side = side_ok && (!bin_ok || (side->complete && side->generation > bin->generation));
        return MESH_CTAB_CARD_USE;
    }
    return bin->exists ? MESH_CTAB_CARD_REFUSED : MESH_CTAB_CARD_FRESH;
}

int mesh_ctab_aside_slot(bool have_0, bool have_1) {
    return !have_0 ? 0 : !have_1 ? 1 : -1;
}
