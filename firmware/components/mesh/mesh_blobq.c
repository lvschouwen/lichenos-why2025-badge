#include "mesh_blobq.h"

#include <string.h>

void mesh_blobq_init(mesh_blobq_t *q) {
    memset(q, 0, sizeof(*q));
}

static mesh_blobq_entry_t *find(mesh_blobq_t const *q, uint8_t const *key) {
    for (size_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        if (q->e[i].len && memcmp(q->e[i].key, key, MESH_BLOBQ_KEY) == 0) {
            return (mesh_blobq_entry_t *)&q->e[i];
        }
    }
    return NULL;
}

static mesh_blobq_entry_t *free_slot(mesh_blobq_t *q) {
    for (size_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        if (!q->e[i].len) {
            return &q->e[i];
        }
    }
    return NULL;
}

bool mesh_blobq_put(mesh_blobq_t *q, uint8_t const *key, size_t key_len, uint8_t const *data, size_t len,
                    uint32_t now_ms) {
    if (key_len < MESH_BLOBQ_KEY || len == 0 || len > MESH_BLOBQ_DATA) {
        return false;
    }
    mesh_blobq_entry_t *e = find(q, key);
    if (!e) {
        e = free_slot(q);
        if (!e) {
            return false;
        }
        memcpy(e->key, key, MESH_BLOBQ_KEY);
        e->since_ms = now_ms;
    }
    memcpy(e->data, data, len);
    e->len = (uint8_t)len;
    e->seq = ++q->next_seq;
    return true;
}

uint8_t mesh_blobq_get(mesh_blobq_t const *q, uint8_t const *key, size_t key_len, uint8_t *dest) {
    mesh_blobq_entry_t const *e = key_len >= MESH_BLOBQ_KEY ? find(q, key) : NULL;
    if (!e) {
        return 0;
    }
    memcpy(dest, e->data, e->len);
    return e->len;
}

bool mesh_blobq_take(mesh_blobq_t const *q, uint32_t now_ms, bool quiet, mesh_blobq_entry_t *out) {
    mesh_blobq_entry_t const *oldest = NULL;
    uint32_t                  waited = 0;
    for (size_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        uint32_t age = now_ms - q->e[i].since_ms; // unsigned: right across the wrap
        if (q->e[i].len && (!oldest || age > waited)) {
            oldest = &q->e[i];
            waited = age;
        }
    }
    if (!oldest || (!quiet && waited < MESH_BLOBQ_MAX_AGE_MS)) {
        return false;
    }
    *out = *oldest;
    return true;
}

void mesh_blobq_done(mesh_blobq_t *q, mesh_blobq_entry_t const *written) {
    mesh_blobq_entry_t *e = find(q, written->key);
    if (e && e->seq == written->seq) {
        e->len = 0;
    }
}

void mesh_blobq_clear(mesh_blobq_t *q) {
    for (size_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        q->e[i].len = 0;
    }
}

size_t mesh_blobq_count(mesh_blobq_t const *q) {
    size_t n = 0;
    for (size_t i = 0; i < MESH_BLOBQ_CAP; i++) {
        n += q->e[i].len != 0;
    }
    return n;
}
