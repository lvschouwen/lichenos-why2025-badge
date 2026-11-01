// The notification history (#62): see notify_ring.h. Pure, host-tested in host_tests/notify_ring_test.c.
#include "notify_ring.h"

#include <string.h>

static void copy(char *dst, size_t cap, char const *src) {
    size_t n = 0;
    if (src) {
        while (n + 1 < cap && src[n]) {
            n++;
        }
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static bool same(char const *field, char const *s) {
    return strcmp(field, s ? s : "") == 0;
}

void nr_init(nr_ring_t *r, uint16_t boot) {
    memset(r, 0, sizeof(*r));
    r->next_id  = 1;
    r->next_seq = 1;
    r->boot     = boot;
}

static uint32_t take_id(nr_ring_t *r) {
    uint32_t id = r->next_id++;
    if (r->next_id == 0) {
        r->next_id = 1; // 4 billion notices: never in practice, but 0 stays "free"
    }
    return id;
}

// The unread entry a new notice may merge into: same (source, target), this boot, recent enough
static nr_entry_t *coalesce_with(nr_ring_t *r, nr_new_t const *n) {
    nr_entry_t *best = NULL;
    for (int i = 0; i < NR_SLOTS; i++) {
        nr_entry_t *e = &r->e[i];
        if (!e->id || e->read || e->boot != r->boot || !same(e->source, n->source) || !same(e->target, n->target)) {
            continue;
        }
        if (n->mono_s - e->mono_s > NR_COALESCE_S) { // unsigned: an older uptime than ours is never "recent"
            continue;
        }
        if (!best || e->seq > best->seq) {
            best = e;
        }
    }
    return best;
}

static nr_entry_t *victim(nr_ring_t *r, nr_new_t const *n) {
    nr_entry_t *pick = NULL;
    for (int i = 0; i < NR_SLOTS; i++) { // the oldest read entry, of any source
        nr_entry_t *e = &r->e[i];
        if (e->read && (!pick || e->seq < pick->seq)) {
            pick = e;
        }
    }
    if (pick) {
        return pick;
    }
    for (int i = 0; i < NR_SLOTS; i++) { // the lowest unread level not above the new one, oldest first
        nr_entry_t *e = &r->e[i];
        if (e->level > n->level || (n->restricted && !same(e->source, n->source))) {
            continue;
        }
        if (!pick || e->level < pick->level || (e->level == pick->level && e->seq < pick->seq)) {
            pick = e;
        }
    }
    return pick;
}

static nr_entry_t *free_slot(nr_ring_t *r) {
    for (int i = 0; i < NR_SLOTS; i++) {
        if (!r->e[i].id) {
            return &r->e[i];
        }
    }
    return NULL;
}

static void stamp(nr_ring_t *r, nr_entry_t *e, nr_new_t const *n) {
    e->seq    = r->next_seq++;
    e->epoch  = n->epoch;
    e->mono_s = n->mono_s;
    e->boot   = r->boot;
    copy(e->body, sizeof(e->body), n->body);
}

nr_outcome_t nr_add(nr_ring_t *r, nr_new_t const *n, uint32_t *id) {
    *id = 0;
    if (n->coalesce) {
        nr_entry_t *e = coalesce_with(r, n);
        if (e) {
            if (e->count < UINT16_MAX) {
                e->count++;
            }
            if (n->level > e->level) {
                e->level = n->level;
            }
            copy(e->title, sizeof(e->title), n->title);
            stamp(r, e, n);
            r->generation++;
            *id = e->id;
            return NR_COALESCED;
        }
    }
    nr_entry_t *e = free_slot(r);
    if (!e) {
        e = victim(r, n);
    }
    if (!e) {
        r->dropped++;
        return NR_DROPPED;
    }
    memset(e, 0, sizeof(*e));
    e->id    = take_id(r);
    e->count = 1;
    e->level = n->level > NR_ERROR ? NR_ERROR : n->level;
    e->flags = n->flags;
    copy(e->source, sizeof(e->source), n->source);
    copy(e->target, sizeof(e->target), n->target);
    copy(e->title, sizeof(e->title), n->title);
    stamp(r, e, n);
    r->generation++;
    *id = e->id;
    return NR_ADDED;
}

nr_entry_t const *nr_get(nr_ring_t const *r, uint32_t id) {
    for (int i = 0; id && i < NR_SLOTS; i++) {
        if (r->e[i].id == id) {
            return &r->e[i];
        }
    }
    return NULL;
}

size_t nr_ids(nr_ring_t const *r, uint32_t *ids, size_t max) {
    // All used slots sorted by seq, newest first (insertion sort over 48), then the first max of them
    uint32_t seqs[NR_SLOTS], all[NR_SLOTS];
    size_t   n = 0;
    for (int i = 0; i < NR_SLOTS; i++) {
        nr_entry_t const *e = &r->e[i];
        if (!e->id) {
            continue;
        }
        size_t at = n++;
        while (at > 0 && seqs[at - 1] < e->seq) {
            seqs[at] = seqs[at - 1];
            all[at]  = all[at - 1];
            at--;
        }
        seqs[at] = e->seq;
        all[at]  = e->id;
    }
    n = n < max ? n : max;
    memcpy(ids, all, n * sizeof(*ids));
    return n;
}

size_t nr_count(nr_ring_t const *r) {
    size_t n = 0;
    for (int i = 0; i < NR_SLOTS; i++) {
        n += r->e[i].id != 0;
    }
    return n;
}

size_t nr_unread(nr_ring_t const *r, char const *source) {
    size_t n = 0;
    for (int i = 0; i < NR_SLOTS; i++) {
        nr_entry_t const *e = &r->e[i];
        n += e->id && !e->read && (!source || same(e->source, source));
    }
    return n;
}

int nr_unread_top(nr_ring_t const *r) {
    int top = -1;
    for (int i = 0; i < NR_SLOTS; i++) {
        nr_entry_t const *e = &r->e[i];
        if (e->id && !e->read && e->level > top) {
            top = e->level;
        }
    }
    return top;
}

static size_t mark_where(nr_ring_t *r, char const *source, char const *target, uint32_t id, int upto) {
    size_t n = 0;
    for (int i = 0; i < NR_SLOTS; i++) {
        nr_entry_t *e = &r->e[i];
        if (!e->id || e->read || (id && e->id != id) || (source && !same(e->source, source)) ||
            (target && !same(e->target, target)) || (upto >= 0 && e->level > upto)) {
            continue;
        }
        e->read = 1;
        n++;
    }
    if (n) {
        r->generation++;
    }
    return n;
}

size_t nr_mark_read(nr_ring_t *r, char const *source, char const *target) {
    return mark_where(r, source, target, 0, -1);
}

size_t nr_mark_read_id(nr_ring_t *r, uint32_t id) {
    return id ? mark_where(r, NULL, NULL, id, -1) : 0;
}

size_t nr_mark_read_upto(nr_ring_t *r, uint8_t level) {
    return mark_where(r, NULL, NULL, 0, level);
}

bool nr_remove(nr_ring_t *r, uint32_t id) {
    nr_entry_t *e = (nr_entry_t *)nr_get(r, id);
    if (!e) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    r->generation++;
    return true;
}

void nr_clear(nr_ring_t *r) {
    memset(r->e, 0, sizeof(r->e));
    r->generation++;
}

// ---- the file -------------------------------------------------------------------------------------------------------

uint32_t nr_crc32(uint8_t const *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static uint8_t *put(uint8_t *p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) {
        *p++ = (uint8_t)(v >> (8 * i));
    }
    return p;
}

static uint64_t get(uint8_t const **p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v |= (uint64_t)(*p)[i] << (8 * i);
    }
    *p += bytes;
    return v;
}

static uint8_t *put_str(uint8_t *p, char const *s, size_t cap) {
    memset(p, 0, cap);
    copy((char *)p, cap, s);
    return p + cap;
}

static bool get_str(uint8_t const **p, char *s, size_t cap) {
    memcpy(s, *p, cap);
    *p += cap;
    return memchr(s, '\0', cap) != NULL;
}

size_t nr_encode(nr_ring_t const *r, uint8_t *buf, size_t cap) {
    uint32_t ids[NR_SLOTS];
    size_t   count = nr_ids(r, ids, NR_SLOTS);
    size_t   size  = NR_HEADER_SIZE + count * NR_RECORD_SIZE + 4;
    if (cap < size) {
        return 0;
    }
    uint8_t *p = buf;
    p          = put(p, NR_FILE_MAGIC, 4);
    p          = put(p, NR_FILE_VERSION, 2);
    p          = put(p, NR_RECORD_SIZE, 2);
    p          = put(p, count, 2);
    p          = put(p, r->boot, 2);
    p          = put(p, r->next_id, 4);
    p          = put(p, r->next_seq, 4);
    p          = put(p, 0, 4);
    for (size_t k = count; k-- > 0;) { // oldest first
        nr_entry_t const *e = nr_get(r, ids[k]);
        p                   = put(p, e->id, 4);
        p                   = put(p, e->seq, 4);
        p                   = put(p, (uint64_t)e->epoch, 8);
        p                   = put(p, e->mono_s, 4);
        p                   = put(p, e->boot, 2);
        p                   = put(p, e->count, 2);
        p                   = put(p, e->level, 1);
        p                   = put(p, e->flags, 1);
        p                   = put(p, e->read, 1);
        p                   = put(p, 0, 1);
        p                   = put_str(p, e->source, NR_SOURCE_CAP);
        p                   = put_str(p, e->target, NR_TARGET_CAP);
        p                   = put_str(p, e->title, NR_TITLE_CAP);
        p                   = put_str(p, e->body, NR_BODY_CAP);
    }
    put(p, nr_crc32(buf, (size_t)(p - buf)), 4);
    return size;
}

static bool decode_entry(uint8_t const **p, nr_entry_t *e) {
    e->id     = (uint32_t)get(p, 4);
    e->seq    = (uint32_t)get(p, 4);
    e->epoch  = (int64_t)get(p, 8);
    e->mono_s = (uint32_t)get(p, 4);
    e->boot   = (uint16_t)get(p, 2);
    e->count  = (uint16_t)get(p, 2);
    e->level  = (uint8_t)get(p, 1);
    e->flags  = (uint8_t)get(p, 1);
    e->read   = (uint8_t)get(p, 1);
    get(p, 1);
    bool strings = get_str(p, e->source, NR_SOURCE_CAP) & get_str(p, e->target, NR_TARGET_CAP) &
                   get_str(p, e->title, NR_TITLE_CAP) & get_str(p, e->body, NR_BODY_CAP);
    return strings && e->id && e->count && e->level <= NR_ERROR && e->read <= 1;
}

bool nr_decode(nr_ring_t *r, uint8_t const *buf, size_t len) {
    nr_init(r, 0);
    if (!buf || len < NR_HEADER_SIZE + 4) {
        return false;
    }
    uint8_t const *p = buf;
    uint32_t magic   = (uint32_t)get(&p, 4);
    uint16_t version = (uint16_t)get(&p, 2);
    uint16_t recsize = (uint16_t)get(&p, 2);
    uint16_t count   = (uint16_t)get(&p, 2);
    uint16_t boot    = (uint16_t)get(&p, 2);
    uint32_t next_id = (uint32_t)get(&p, 4);
    uint32_t seq     = (uint32_t)get(&p, 4);
    get(&p, 4);
    if (magic != NR_FILE_MAGIC || version != NR_FILE_VERSION || recsize != NR_RECORD_SIZE || count > NR_SLOTS ||
        len != NR_HEADER_SIZE + (size_t)count * NR_RECORD_SIZE + 4) {
        return false;
    }
    uint8_t const *crc_at = buf + len - 4;
    if ((uint32_t)get(&crc_at, 4) != nr_crc32(buf, len - 4)) {
        return false;
    }
    // Straight into r (no second 11 KB copy on a task's stack); anything wrong leaves it empty again
    r->boot     = boot;
    r->next_id  = next_id ? next_id : 1;
    r->next_seq = seq ? seq : 1;
    for (uint16_t i = 0; i < count; i++) {
        nr_entry_t *e = &r->e[i];
        if (!decode_entry(&p, e) || nr_get(r, e->id) != e) { // a duplicate id is a damaged file too
            nr_init(r, 0);
            return false;
        }
        if (e->id >= r->next_id) {
            r->next_id = e->id + 1;
        }
        if (e->seq >= r->next_seq) {
            r->next_seq = e->seq + 1;
        }
    }
    return true;
}
