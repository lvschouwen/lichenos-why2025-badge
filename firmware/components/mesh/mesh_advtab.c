#include "mesh_advtab.h"

#include <string.h>

#define FORMAT 1
static char const MAGIC[4] = {'L', 'A', 'D', 'V'};

size_t mesh_advtab_bytes(size_t cap) {
    return cap * (MESH_ADVTAB_KEY + 1 + MESH_ADVTAB_DATA);
}

void mesh_advtab_init(mesh_advtab_t *t, void *mem, size_t cap) {
    memset(t, 0, sizeof(*t));
    t->keys = (uint8_t *)mem;
    t->lens = t->keys + cap * MESH_ADVTAB_KEY;
    t->data = t->lens + cap;
    t->cap  = cap;
    memset(t->lens, 0, cap);
}

#define NONE ((size_t)-1)

static size_t find(mesh_advtab_t const *t, uint8_t const *key) {
    for (size_t i = 0; i < t->top; i++) {
        if (t->lens[i] && memcmp(t->keys + i * MESH_ADVTAB_KEY, key, MESH_ADVTAB_KEY) == 0) {
            return i;
        }
    }
    return NONE;
}

static size_t free_slot(mesh_advtab_t *t) {
    if (t->count < t->top) { // a hole below the top: a removed node's slot
        for (size_t i = 0; i < t->top; i++) {
            if (!t->lens[i]) {
                return i;
            }
        }
    }
    return t->top < t->cap ? t->top++ : NONE;
}

static void drop(mesh_advtab_t *t, size_t i) {
    t->lens[i] = 0;
    t->count--;
    t->changes++;
}

bool mesh_advtab_put(mesh_advtab_t *t, uint8_t const *key, size_t key_len, uint8_t const *data, size_t len) {
    if (key_len < MESH_ADVTAB_KEY || len < MESH_ADVTAB_MIN || len > MESH_ADVTAB_DATA) {
        return false;
    }
    size_t i = find(t, key);
    if (i == NONE) {
        i = free_slot(t);
        if (i == NONE) {
            t->refused++;
            return false;
        }
        memcpy(t->keys + i * MESH_ADVTAB_KEY, key, MESH_ADVTAB_KEY);
        t->count++;
    }
    memcpy(t->data + i * MESH_ADVTAB_DATA, data, len);
    t->lens[i] = (uint8_t)len;
    t->changes++;
    return true;
}

uint8_t mesh_advtab_get(mesh_advtab_t const *t, uint8_t const *key, size_t key_len, uint8_t *dest) {
    size_t i = key_len >= MESH_ADVTAB_KEY ? find(t, key) : NONE;
    if (i == NONE) {
        return 0;
    }
    memcpy(dest, t->data + i * MESH_ADVTAB_DATA, t->lens[i]);
    return t->lens[i];
}

bool mesh_advtab_remove(mesh_advtab_t *t, uint8_t const *key, size_t key_len) {
    size_t i = key_len >= MESH_ADVTAB_KEY ? find(t, key) : NONE;
    if (i == NONE) {
        return false;
    }
    drop(t, i);
    return true;
}

void mesh_advtab_clear(mesh_advtab_t *t) {
    memset(t->lens, 0, t->cap);
    t->top   = 0;
    t->count = 0;
    t->changes++;
}

size_t mesh_advtab_prune(mesh_advtab_t *t, bool (*keep)(uint8_t const key[MESH_ADVTAB_KEY], void *ctx), void *ctx) {
    size_t gone = 0;
    for (size_t i = 0; i < t->top; i++) {
        if (t->lens[i] && !keep(t->keys + i * MESH_ADVTAB_KEY, ctx)) {
            drop(t, i);
            gone++;
        }
    }
    return gone;
}

// ---- the snapshot ---------------------------------------------------------------------------------------------------

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// zlib's CRC-32, a nibble at a time: 16 words of table, fast enough for a megabyte every 15 minutes
static uint32_t crc32(uint8_t const *p, size_t n) {
    static uint32_t const T[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                   0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                   0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    uint32_t              c     = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        c = T[c & 15] ^ (c >> 4);
        c = T[c & 15] ^ (c >> 4);
    }
    return c ^ 0xffffffffu;
}

size_t mesh_advtab_pack_size(mesh_advtab_t const *t) {
    size_t n = MESH_ADVTAB_HEAD;
    for (size_t i = 0; i < t->top; i++) {
        n += t->lens[i] ? MESH_ADVTAB_REC + t->lens[i] : 0;
    }
    return n;
}

size_t mesh_advtab_pack(mesh_advtab_t const *t, uint32_t generation, uint8_t *buf, size_t cap) {
    size_t n = mesh_advtab_pack_size(t);
    if (n > cap) {
        return 0;
    }
    uint8_t *p = buf + MESH_ADVTAB_HEAD;
    for (size_t i = 0; i < t->top; i++) {
        if (t->lens[i]) {
            memcpy(p, t->keys + i * MESH_ADVTAB_KEY, MESH_ADVTAB_KEY);
            p[MESH_ADVTAB_KEY] = t->lens[i];
            memcpy(p + MESH_ADVTAB_REC, t->data + i * MESH_ADVTAB_DATA, t->lens[i]);
            p += MESH_ADVTAB_REC + t->lens[i];
        }
    }
    memcpy(buf, MAGIC, sizeof(MAGIC));
    buf[4] = FORMAT;
    buf[5] = buf[6] = buf[7] = 0;
    put32(buf + 8, (uint32_t)t->count);
    put32(buf + 12, (uint32_t)(n - MESH_ADVTAB_HEAD));
    put32(buf + 16, 0);
    put32(buf + 20, generation);
    return n;
}

void mesh_advtab_seal(uint8_t *buf, size_t len) {
    if (len >= MESH_ADVTAB_HEAD) {
        put32(buf + 16, crc32(buf + MESH_ADVTAB_HEAD, len - MESH_ADVTAB_HEAD));
    }
}

// The records add up to exactly the body and their number is the header's
static bool records_fit(uint8_t const *body, size_t body_len, uint32_t count) {
    size_t at = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (body_len - at < MESH_ADVTAB_REC || body[at + MESH_ADVTAB_KEY] < MESH_ADVTAB_MIN) {
            return false;
        }
        size_t rec = MESH_ADVTAB_REC + body[at + MESH_ADVTAB_KEY];
        if (body_len - at < rec) {
            return false;
        }
        at += rec;
    }
    return at == body_len;
}

bool mesh_advtab_valid(uint8_t const *buf, size_t len, uint32_t *generation) {
    if (len < MESH_ADVTAB_HEAD || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0 || buf[4] != FORMAT || buf[5] != 0) {
        return false;
    }
    size_t body_len = get32(buf + 12);
    if (body_len != len - MESH_ADVTAB_HEAD || crc32(buf + MESH_ADVTAB_HEAD, body_len) != get32(buf + 16) ||
        !records_fit(buf + MESH_ADVTAB_HEAD, body_len, get32(buf + 8))) {
        return false;
    }
    *generation = get32(buf + 20);
    return true;
}

size_t mesh_advtab_unpack(mesh_advtab_t *t, uint8_t const *buf, size_t len) {
    uint32_t generation;
    mesh_advtab_clear(t);
    if (!mesh_advtab_valid(buf, len, &generation)) {
        return 0;
    }
    uint32_t       count = get32(buf + 8);
    uint8_t const *p     = buf + MESH_ADVTAB_HEAD;
    for (uint32_t i = 0; i < count; i++) {
        mesh_advtab_put(t, p, MESH_ADVTAB_KEY, p + MESH_ADVTAB_REC, p[MESH_ADVTAB_KEY]);
        p += MESH_ADVTAB_REC + p[MESH_ADVTAB_KEY];
    }
    return t->count;
}

bool mesh_advtab_due(mesh_advtab_clock_t const *c) {
    if (!c->changed) {
        return false;
    }
    if (c->forced) {
        return c->now_s - c->last_try_s >= MESH_ADVTAB_RETRY_S;
    }
    if (c->on_card) {
        return c->now_s - c->last_try_s >= MESH_ADVTAB_CARD_S;
    }
    return c->now_s - c->last_try_s >= MESH_ADVTAB_INTERNAL_S && mesh_advtab_internal_ok(c, false);
}

bool mesh_advtab_internal_ok(mesh_advtab_clock_t const *c, bool at_stop) {
    return at_stop || c->forced || !c->internal_written || c->now_s - c->last_internal_s >= MESH_ADVTAB_INTERNAL_S;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c |= 0x20;
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool mesh_advtab_key_from_name(char const *name, uint8_t key[MESH_ADVTAB_KEY]) {
    if (strlen(name) != 2 * MESH_ADVTAB_KEY) {
        return false;
    }
    for (size_t i = 0; i < MESH_ADVTAB_KEY; i++) {
        int hi = hex_digit(name[2 * i]), lo = hex_digit(name[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        key[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

bool mesh_advtab_walk_step(mesh_advtab_t *t, mesh_advtab_walk_t *w, mesh_advtab_walk_ops_t const *ops, void *ctx,
                           size_t max_names) {
    uint8_t key[MESH_ADVTAB_KEY], data[MESH_ADVTAB_DATA];
    if (w->done) {
        return true;
    }
    for (size_t i = 0; i < max_names; i++) {
        char const *name = ops->next(ctx);
        if (!name) {
            w->done = true;
            return true;
        }
        w->names++;
        if (!mesh_advtab_key_from_name(name, key)) {
            continue;
        }
        ops->lock(ctx);
        uint8_t have = mesh_advtab_get(t, key, sizeof(key), data);
        ops->unlock(ctx);
        if (have) {
            continue; // the snapshot (or an advert since) has the node: it is the newer
        }
        size_t n = ops->read(ctx, name, data);
        if (n == 0) {
            continue;
        }
        ops->lock(ctx);
        // an advert may have put the node while the file was read: the file is older, it does not replace it
        bool put = !mesh_advtab_get(t, key, sizeof(key), (uint8_t[MESH_ADVTAB_DATA]){0}) &&
                   mesh_advtab_put(t, key, sizeof(key), data, n);
        ops->unlock(ctx);
        w->taken_over += put ? 1 : 0;
    }
    return false;
}
