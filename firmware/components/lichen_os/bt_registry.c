#include "bt_registry.h"

#include <string.h>

// A handle: generation in the top 24 bits, slot + 1 in the low 8, so 0 is never valid
#define H_SLOT(h)    ((int)((h) & 0xffu) - 1)
#define H_GEN(h)     ((h) >> 8)
#define H_MAKE(g, s) (((g) << 8) | ((uint32_t)(s) + 1u))
#define GEN_MASK     0xffffffu

void bt_reg_init(bt_reg_t *r) {
    memset(r, 0, sizeof(*r));
    r->gen = 1;
}

void bt_reg_host_started(bt_reg_t *r) {
    r->started = true;
}

static uint32_t next_gen(bt_reg_t *r) {
    uint32_t g = r->gen & GEN_MASK;
    if (g == 0) {
        g = 1;
    }
    r->gen = (g + 1u) & GEN_MASK;
    return g;
}

// ---- Owners ----

static bt_reg_owner_t const *owner_at(bt_reg_t const *r, bt_owner_t owner) {
    int s = H_SLOT(owner);
    if (owner == 0 || s < 0 || s >= (int)BT_REG_OWNERS) {
        return NULL;
    }
    bt_reg_owner_t const *o = &r->owners[s];
    return o->used && o->gen == H_GEN(owner) ? o : NULL;
}

static bt_reg_owner_t *owner_mut(bt_reg_t *r, bt_owner_t owner) {
    return (bt_reg_owner_t *)owner_at(r, owner);
}

bt_owner_t bt_reg_owner_add(bt_reg_t *r, bt_owner_kind_t kind) {
    for (unsigned i = 0; i < BT_REG_OWNERS; i++) {
        bt_reg_owner_t *o = &r->owners[i];
        if (!o->used) {
            memset(o, 0, sizeof(*o));
            o->used = true;
            o->kind = kind;
            o->gen  = next_gen(r);
            return H_MAKE(o->gen, i);
        }
    }
    return 0;
}

bool bt_reg_owner_valid(bt_reg_t const *r, bt_owner_t owner) {
    bt_reg_owner_t const *o = owner_at(r, owner);
    return o && !o->revoked;
}

static size_t revoke_services(bt_reg_t *r, bt_owner_t owner, bt_undo_t *undo, size_t n, size_t cap) {
    for (unsigned i = 0; i < BT_REG_SERVICES && n < cap; i++) {
        bt_reg_svc_t *s = &r->svcs[i];
        if (s->used && s->owner == owner) {
            s->removing = true;
            undo[n++]   = (bt_undo_t){.kind = BT_UNDO_REMOVE_SERVICE, .svc = H_MAKE(s->gen, i)};
        }
    }
    return n;
}

// The owner's channels close (confirmed by bt_reg_chan_gone); its PSMs are freed at once
static size_t revoke_l2cap(bt_reg_t *r, bt_owner_t owner, bt_undo_t *undo, size_t n, size_t cap) {
    for (unsigned i = 0; i < BT_REG_CHANS && n < cap; i++) {
        bt_reg_chan_t *c = &r->chans[i];
        if (c->used && c->owner == owner) {
            c->closing = true;
            undo[n++]  = (bt_undo_t){.kind = BT_UNDO_CLOSE_CHAN, .handle = c->cid};
        }
    }
    for (unsigned i = 0; i < BT_REG_LISTENS && n < cap; i++) {
        bt_reg_listen_t *l = &r->listens[i];
        if (l->used && l->owner == owner) {
            undo[n++] = (bt_undo_t){.kind = BT_UNDO_UNLISTEN, .handle = l->psm};
            memset(l, 0, sizeof(*l));
        }
    }
    return n;
}

size_t bt_reg_revoke(bt_reg_t *r, bt_owner_t owner, bt_undo_t *undo, size_t cap) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (!o) {
        return 0;
    }
    o->revoked = true;
    size_t n   = revoke_services(r, owner, undo, 0, cap);
    if (o->scanning && n < cap) {
        undo[n++] = (bt_undo_t){.kind = BT_UNDO_STOP_SCAN};
    }
    for (unsigned i = 0; i < BT_REG_CONNS && n < cap; i++) {
        bt_reg_conn_t *c = &r->conns[i];
        if (c->used && c->owner == owner) {
            c->closing = true;
            undo[n++]  = (bt_undo_t){.kind = BT_UNDO_DISCONNECT, .handle = c->handle};
        }
    }
    return revoke_l2cap(r, owner, undo, n, cap);
}

static bool l2cap_idle(bt_reg_t const *r, bt_owner_t owner) {
    for (unsigned i = 0; i < BT_REG_CHANS; i++) {
        if (r->chans[i].used && r->chans[i].owner == owner) {
            return false;
        }
    }
    for (unsigned i = 0; i < BT_REG_LISTENS; i++) {
        if (r->listens[i].used && r->listens[i].owner == owner) {
            return false;
        }
    }
    return true;
}

bool bt_reg_owner_idle(bt_reg_t const *r, bt_owner_t owner) {
    bt_reg_owner_t const *o = owner_at(r, owner);
    if (!o) {
        return true; // released already: nothing can be live
    }
    if (o->scanning || o->events > 0) {
        return false;
    }
    for (unsigned i = 0; i < BT_REG_SERVICES; i++) {
        if (r->svcs[i].used && r->svcs[i].owner == owner) {
            return false;
        }
    }
    for (unsigned i = 0; i < BT_REG_CONNS; i++) {
        if (r->conns[i].used && r->conns[i].owner == owner) {
            return false;
        }
    }
    return l2cap_idle(r, owner);
}

bool bt_reg_owner_release(bt_reg_t *r, bt_owner_t owner) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (!o || !o->revoked || !bt_reg_owner_idle(r, owner)) {
        return false;
    }
    memset(o, 0, sizeof(*o));
    return true;
}

// ---- GATT services ----

size_t bt_reg_static_services(bt_reg_t const *r, lichen_bt_svc_t *out, size_t cap) {
    size_t n = 0;
    for (unsigned i = 0; i < BT_REG_SERVICES && n < cap; i++) {
        if (r->svcs[i].used && r->svcs[i].is_static) {
            out[n++] = H_MAKE(r->svcs[i].gen, i);
        }
    }
    return n;
}

static bool uuid_ok(lichen_bt_uuid_t const *u) {
    return u->len == 2 || u->len == 16;
}

static bool uuid_eq(lichen_bt_uuid_t const *a, lichen_bt_uuid_t const *b) {
    return a->len == b->len && memcmp(a->b, b->b, a->len) == 0;
}

// GAP and GATT are the stack's; the peer service is the OS's
static bool uuid_reserved(lichen_bt_uuid_t const *u, bt_owner_kind_t kind) {
    static lichen_bt_uuid_t const peer = LICHEN_BT_PEER_UUID;
    if (u->len == 2 && u->b[1] == 0x18 && (u->b[0] == 0x00 || u->b[0] == 0x01)) {
        return true;
    }
    return kind != BT_OWNER_OS && uuid_eq(u, &peer);
}

static bool chr_def_ok(lichen_bt_chr_def_t const *c) {
    return uuid_ok(&c->uuid) && c->props != 0 && (c->props & ~LICHEN_BT_PROP_ALL) == 0 && c->max_len >= 1 &&
           c->max_len <= LICHEN_BT_VALUE_MAX && c->level <= LICHEN_BT_BONDED;
}

static lichen_bt_err_t svc_def_check(bt_reg_t const *r, bt_owner_kind_t kind, lichen_bt_svc_def_t const *def) {
    if (!def || !uuid_ok(&def->uuid) || uuid_reserved(&def->uuid, kind) || !def->chrs || def->chr_count == 0 ||
        def->chr_count > LICHEN_BT_SVC_CHRS_MAX) {
        return LICHEN_BT_INVALID;
    }
    for (unsigned i = 0; i < def->chr_count; i++) {
        if (!chr_def_ok(&def->chrs[i])) {
            return LICHEN_BT_INVALID;
        }
    }
    for (unsigned i = 0; i < BT_REG_SERVICES; i++) {
        if (r->svcs[i].used && uuid_eq(&r->svcs[i].uuid, &def->uuid)) {
            return LICHEN_BT_EXISTS; // a removing one too: the stack may still serve it
        }
    }
    return LICHEN_BT_OK;
}

static int free_svc_slot(bt_reg_t const *r) {
    for (unsigned i = 0; i < BT_REG_SERVICES; i++) {
        if (!r->svcs[i].used) {
            return (int)i;
        }
    }
    return -1;
}

// Room in the OS's or the apps' budget for one more service of n characteristics
static bool budget_ok(bt_reg_t const *r, bool os, unsigned n) {
    unsigned svcs = 0, chrs = 0;
    for (unsigned i = 0; i < BT_REG_SERVICES; i++) {
        if (r->svcs[i].used && r->svcs[i].os == os) {
            svcs++;
            chrs += r->svcs[i].chr_count;
        }
    }
    return svcs < (os ? BT_REG_OS_SERVICES : BT_REG_APP_SERVICES) &&
           chrs + n <= (os ? BT_REG_OS_CHRS : BT_REG_APP_CHRS);
}

static lichen_bt_chr_t chr_take(bt_reg_t *r, unsigned svc, lichen_bt_chr_def_t const *d) {
    for (unsigned i = 0; i < BT_REG_CHRS; i++) {
        bt_reg_chr_t *c = &r->chrs[i];
        if (!c->used) {
            *c = (bt_reg_chr_t){.used    = true,
                                .gen     = next_gen(r),
                                .svc     = (uint8_t)svc,
                                .uuid    = d->uuid,
                                .props   = d->props,
                                .level   = d->level,
                                .max_len = d->max_len};
            return H_MAKE(c->gen, i);
        }
    }
    return 0; // not reached: the caller counted the free slots
}

lichen_bt_err_t bt_reg_service_add(
    bt_reg_t *r, bt_owner_t owner, lichen_bt_svc_def_t const *def, lichen_bt_svc_t *svc, lichen_bt_chr_t *chrs_out
) {
    bt_reg_owner_t const *o = owner_at(r, owner);
    if (!o || o->revoked) {
        return LICHEN_BT_NOT_FOUND;
    }
    lichen_bt_err_t e = svc_def_check(r, o->kind, def);
    if (e != LICHEN_BT_OK) {
        return e;
    }
    bool os   = o->kind == BT_OWNER_OS;
    int  slot = free_svc_slot(r);
    if (slot < 0 || !budget_ok(r, os, def->chr_count)) {
        return LICHEN_BT_FULL; // the budgets add up to the tables, so a free slot is always there within one
    }
    bt_reg_svc_t *s = &r->svcs[slot];
    *s              = (bt_reg_svc_t){.used      = true,
                                     .advertise = def->advertise,
                                     .gen       = next_gen(r),
                                     .is_static = os && !r->started,
                                     .os        = os,
                                     .owner     = owner,
                                     .uuid      = def->uuid,
                                     .chr_count = def->chr_count};
    for (unsigned i = 0; i < def->chr_count; i++) {
        chrs_out[i] = chr_take(r, (unsigned)slot, &def->chrs[i]);
    }
    *svc = H_MAKE(s->gen, slot);
    return LICHEN_BT_OK;
}

static bt_reg_svc_t *svc_at(bt_reg_t *r, lichen_bt_svc_t svc) {
    int s = H_SLOT(svc);
    if (svc == 0 || s < 0 || s >= (int)BT_REG_SERVICES) {
        return NULL;
    }
    bt_reg_svc_t *p = &r->svcs[s];
    return p->used && p->gen == H_GEN(svc) ? p : NULL;
}

lichen_bt_err_t bt_reg_service_remove(bt_reg_t *r, bt_owner_t owner, lichen_bt_svc_t svc, bt_undo_t *undo) {
    bt_reg_svc_t *s = svc_at(r, svc);
    if (!bt_reg_owner_valid(r, owner) || !s || s->owner != owner || s->removing) {
        return LICHEN_BT_NOT_FOUND;
    }
    if (s->is_static) {
        return LICHEN_BT_INVALID;
    }
    s->removing = true;
    *undo       = (bt_undo_t){.kind = BT_UNDO_REMOVE_SERVICE, .svc = svc};
    return LICHEN_BT_OK;
}

void bt_reg_service_gone(bt_reg_t *r, lichen_bt_svc_t svc) {
    bt_reg_svc_t *s = svc_at(r, svc);
    if (!s) {
        return;
    }
    unsigned slot = (unsigned)H_SLOT(svc);
    for (unsigned i = 0; i < BT_REG_CHRS; i++) {
        if (r->chrs[i].used && r->chrs[i].svc == slot) {
            memset(&r->chrs[i], 0, sizeof(r->chrs[i]));
        }
    }
    memset(s, 0, sizeof(*s));
}

static bt_reg_chr_t const *chr_at(bt_reg_t const *r, lichen_bt_chr_t chr) {
    int s = H_SLOT(chr);
    if (chr == 0 || s < 0 || s >= (int)BT_REG_CHRS) {
        return NULL;
    }
    bt_reg_chr_t const *c = &r->chrs[s];
    return c->used && c->gen == H_GEN(chr) ? c : NULL;
}

bool bt_reg_chr_set_handle(bt_reg_t *r, lichen_bt_chr_t chr, uint16_t att_handle) {
    bt_reg_chr_t *c = (bt_reg_chr_t *)chr_at(r, chr);
    if (!c || att_handle == 0) {
        return false;
    }
    c->att_handle = att_handle;
    return true;
}

// Fills out for a characteristic of a live service whose owner was not revoked
static bool chr_fill(bt_reg_t const *r, unsigned i, bt_chr_info_t *out) {
    bt_reg_chr_t const *c = &r->chrs[i];
    bt_reg_svc_t const *s = &r->svcs[c->svc];
    if (!c->used || !s->used || s->removing || !bt_reg_owner_valid(r, s->owner)) {
        return false;
    }
    *out = (bt_chr_info_t){.chr        = H_MAKE(c->gen, i),
                           .svc        = H_MAKE(s->gen, c->svc),
                           .owner      = s->owner,
                           .uuid       = c->uuid,
                           .props      = c->props,
                           .level      = c->level,
                           .max_len    = c->max_len,
                           .att_handle = c->att_handle};
    return true;
}

bool bt_reg_chr_by_handle(bt_reg_t const *r, uint16_t att_handle, bt_chr_info_t *out) {
    if (att_handle == 0) {
        return false;
    }
    for (unsigned i = 0; i < BT_REG_CHRS; i++) {
        if (r->chrs[i].used && r->chrs[i].att_handle == att_handle) {
            return chr_fill(r, i, out);
        }
    }
    return false;
}

bool bt_reg_chr_info(bt_reg_t const *r, bt_owner_t owner, lichen_bt_chr_t chr, bt_chr_info_t *out) {
    bt_chr_info_t got;
    if (!chr_at(r, chr) || !chr_fill(r, (unsigned)H_SLOT(chr), &got) || got.owner != owner) {
        return false;
    }
    *out = got;
    return true;
}

// ---- Scans, connections, events ----

lichen_bt_err_t bt_reg_scan_claim(bt_reg_t *r, bt_owner_t owner) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (!o || o->revoked) {
        return LICHEN_BT_NOT_FOUND;
    }
    for (unsigned i = 0; i < BT_REG_OWNERS; i++) {
        if (r->owners[i].used && r->owners[i].scanning) {
            return LICHEN_BT_BUSY; // the owner's own running scan too: one start per scan
        }
    }
    o->scanning = true;
    return LICHEN_BT_OK;
}

void bt_reg_scan_done(bt_reg_t *r, bt_owner_t owner) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (o) {
        o->scanning = false;
    }
}

lichen_bt_err_t bt_reg_conn_add(bt_reg_t *r, bt_owner_t owner, uint16_t handle) {
    if (!bt_reg_owner_valid(r, owner)) {
        return LICHEN_BT_NOT_FOUND;
    }
    for (unsigned i = 0; i < BT_REG_CONNS; i++) {
        if (r->conns[i].used && r->conns[i].handle == handle) {
            return LICHEN_BT_EXISTS;
        }
    }
    for (unsigned i = 0; i < BT_REG_CONNS; i++) {
        if (!r->conns[i].used) {
            r->conns[i] = (bt_reg_conn_t){.used = true, .owner = owner, .handle = handle};
            return LICHEN_BT_OK;
        }
    }
    return LICHEN_BT_FULL;
}

bt_owner_t bt_reg_conn_owner(bt_reg_t const *r, uint16_t handle) {
    for (unsigned i = 0; i < BT_REG_CONNS; i++) {
        bt_reg_conn_t const *c = &r->conns[i];
        if (c->used && c->handle == handle) {
            return c->closing || !bt_reg_owner_valid(r, c->owner) ? 0 : c->owner;
        }
    }
    return 0;
}

void bt_reg_conn_gone(bt_reg_t *r, uint16_t handle) {
    for (unsigned i = 0; i < BT_REG_CONNS; i++) {
        if (r->conns[i].used && r->conns[i].handle == handle) {
            memset(&r->conns[i], 0, sizeof(r->conns[i]));
        }
    }
}

// ---- L2CAP ----

lichen_bt_err_t bt_reg_listen(bt_reg_t *r, bt_owner_t owner, uint16_t psm) {
    bt_reg_owner_t const *o = owner_at(r, owner);
    if (!o || o->revoked) {
        return LICHEN_BT_NOT_FOUND;
    }
    if (psm < LICHEN_BT_PSM_MIN || psm > LICHEN_BT_PSM_MAX || (psm == LICHEN_BT_PSM_PEER && o->kind != BT_OWNER_OS)) {
        return LICHEN_BT_INVALID;
    }
    if (bt_reg_listener(r, psm) != 0) {
        return LICHEN_BT_EXISTS;
    }
    for (unsigned i = 0; i < BT_REG_LISTENS; i++) {
        if (!r->listens[i].used) {
            r->listens[i] = (bt_reg_listen_t){.used = true, .owner = owner, .psm = psm};
            return LICHEN_BT_OK;
        }
    }
    return LICHEN_BT_FULL;
}

void bt_reg_unlisten(bt_reg_t *r, bt_owner_t owner, uint16_t psm) {
    for (unsigned i = 0; i < BT_REG_LISTENS; i++) {
        if (r->listens[i].used && r->listens[i].owner == owner && r->listens[i].psm == psm) {
            memset(&r->listens[i], 0, sizeof(r->listens[i]));
        }
    }
}

bt_owner_t bt_reg_listener(bt_reg_t const *r, uint16_t psm) {
    for (unsigned i = 0; i < BT_REG_LISTENS; i++) {
        bt_reg_listen_t const *l = &r->listens[i];
        if (l->used && l->psm == psm) {
            return l->owner; // a stale (revoked) owner's PSMs were freed by the revoke
        }
    }
    return 0;
}

lichen_bt_err_t bt_reg_chan_add(bt_reg_t *r, bt_owner_t owner, uint16_t cid) {
    if (!bt_reg_owner_valid(r, owner)) {
        return LICHEN_BT_NOT_FOUND;
    }
    int free_slot = -1;
    for (unsigned i = 0; i < BT_REG_CHANS; i++) {
        if (r->chans[i].used && r->chans[i].cid == cid) {
            return LICHEN_BT_EXISTS;
        }
        if (!r->chans[i].used && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) {
        return LICHEN_BT_FULL;
    }
    r->chans[free_slot] = (bt_reg_chan_t){.used = true, .owner = owner, .cid = cid};
    return LICHEN_BT_OK;
}

bt_owner_t bt_reg_chan_owner(bt_reg_t const *r, uint16_t cid) {
    for (unsigned i = 0; i < BT_REG_CHANS; i++) {
        bt_reg_chan_t const *c = &r->chans[i];
        if (c->used && c->cid == cid) {
            return c->closing || !bt_reg_owner_valid(r, c->owner) ? 0 : c->owner;
        }
    }
    return 0;
}

void bt_reg_chan_gone(bt_reg_t *r, uint16_t cid) {
    for (unsigned i = 0; i < BT_REG_CHANS; i++) {
        if (r->chans[i].used && r->chans[i].cid == cid) {
            memset(&r->chans[i], 0, sizeof(r->chans[i]));
        }
    }
}

bool bt_reg_event_queued(bt_reg_t *r, bt_owner_t owner) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (!o || o->revoked || o->events == UINT16_MAX) {
        return false;
    }
    o->events++;
    return true;
}

void bt_reg_event_done(bt_reg_t *r, bt_owner_t owner) {
    bt_reg_owner_t *o = owner_mut(r, owner);
    if (o && o->events > 0) {
        o->events--;
    }
}

// ---- The advert ----

static size_t advert_pass(bt_reg_t const *r, bool os, lichen_bt_uuid_t *out, size_t n, size_t cap) {
    for (unsigned i = 0; i < BT_REG_SERVICES && n < cap; i++) {
        bt_reg_svc_t const   *s = &r->svcs[i];
        bt_reg_owner_t const *o = owner_at(r, s->owner);
        if (s->used && !s->removing && s->advertise && o && !o->revoked && (o->kind == BT_OWNER_OS) == os) {
            out[n++] = s->uuid;
        }
    }
    return n;
}

size_t bt_reg_advert_uuids(bt_reg_t const *r, lichen_bt_uuid_t *out, size_t cap) {
    return advert_pass(r, false, out, advert_pass(r, true, out, 0, cap), cap);
}

// One buffer being packed: what it has used and which UUIDs (by index) go in its two lists
typedef struct {
    uint8_t *buf;
    size_t   used;
    uint8_t  n16, n128;
    uint8_t  i16[BT_ADV_MAX / 2], i128[1];
} adv_buf_t;

static void put_ad(adv_buf_t *b, uint8_t type, void const *data, size_t len) {
    b->buf[b->used++] = (uint8_t)(len + 1u);
    b->buf[b->used++] = type;
    memcpy(b->buf + b->used, data, len);
    b->used += len;
}

// The name as one AD structure in what is left: complete (0x09), or shortened (0x08) at a UTF-8 character boundary
static void put_name(adv_buf_t *b, char const *name) {
    size_t len  = name ? strlen(name) : 0;
    size_t room = BT_ADV_MAX - b->used;
    if (len == 0 || room < 3) {
        return;
    }
    if (len + 2 <= room) {
        put_ad(b, 0x09, name, len);
        return;
    }
    size_t cut = room - 2;
    while (cut > 0 && ((uint8_t)name[cut] & 0xc0u) == 0x80u) {
        cut--; // name[cut] continues a character: cut before that character's lead byte
    }
    if (cut > 0) {
        put_ad(b, 0x08, name, cut);
    }
}

// What uuid would add to b: its bytes, plus 2 for the list's header when it is the first of its size
static size_t uuid_cost(adv_buf_t const *b, lichen_bt_uuid_t const *u) {
    uint8_t have = u->len == 2 ? b->n16 : b->n128;
    return u->len + (have ? 0u : 2u);
}

static bool uuid_try(adv_buf_t *b, lichen_bt_uuid_t const *u, uint8_t idx) {
    if (b->used + uuid_cost(b, u) > BT_ADV_MAX || (u->len == 16 && b->n128 >= 1)) {
        return false;
    }
    b->used += uuid_cost(b, u);
    if (u->len == 2) {
        b->i16[b->n16++] = idx;
    } else {
        b->i128[b->n128++] = idx;
    }
    return true;
}

// Writes the lists chosen for b at its end (b->used counted them already)
static void put_lists(adv_buf_t *b, size_t start, lichen_bt_uuid_t const *uuids, size_t all16, size_t all128) {
    b->used = start;
    if (b->n16) {
        uint8_t d[BT_ADV_MAX];
        for (unsigned i = 0; i < b->n16; i++) {
            memcpy(d + 2 * i, uuids[b->i16[i]].b, 2);
        }
        put_ad(b, b->n16 == all16 ? 0x03 : 0x02, d, 2u * b->n16);
    }
    if (b->n128) {
        put_ad(b, b->n128 == all128 ? 0x07 : 0x06, uuids[b->i128[0]].b, 16);
    }
}

static bool uuids_check(lichen_bt_uuid_t const *uuids, size_t n, size_t *all16, size_t *all128) {
    *all16 = *all128 = 0;
    for (size_t i = 0; i < n; i++) {
        if (!uuid_ok(&uuids[i])) {
            return false;
        }
        *(uuids[i].len == 2 ? all16 : all128) += 1;
    }
    return true;
}

bool bt_adv_pack(
    uint8_t flags, char const *name, lichen_bt_uuid_t const *uuids, size_t n, size_t rotation, bt_adv_t *out
) {
    size_t all16, all128;
    memset(out, 0, sizeof(*out));
    if (n > UINT8_MAX || !uuids_check(uuids, n, &all16, &all128)) {
        return false;
    }
    adv_buf_t a = {.buf = out->adv}, s = {.buf = out->rsp};
    if (flags) {
        put_ad(&a, 0x01, &flags, 1);
    }
    put_name(&a, name);
    size_t a_start = a.used;
    for (size_t k = 0; k < n; k++) {
        uint8_t i = (uint8_t)((rotation + k) % n);
        if (uuid_try(&a, &uuids[i], i) || uuid_try(&s, &uuids[i], i)) {
            out->placed++;
        }
    }
    put_lists(&a, a_start, uuids, all16, all128);
    put_lists(&s, 0, uuids, all16, all128);
    out->adv_len = (uint8_t)a.used;
    out->rsp_len = (uint8_t)s.used;
    out->partial = out->placed < n;
    return true;
}
