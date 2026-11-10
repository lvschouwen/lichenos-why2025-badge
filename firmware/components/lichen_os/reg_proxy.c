#include "reg_proxy.h"

#include <string.h>

static reg_proxy_slot_t *find(reg_proxy_t *p, uint16_t id) {
    for (size_t i = 0; i < p->n; i++) {
        if (p->slot[i].id == id) {
            return &p->slot[i];
        }
    }
    return NULL;
}

reg_proxy_slot_t const *reg_proxy_slot(reg_proxy_t const *p, uint16_t id) {
    return find((reg_proxy_t *)p, id);
}

bool reg_proxy_init(reg_proxy_t *p, uint16_t const *ids, uint8_t const *types, size_t n, uint32_t now_ms) {
    memset(p, 0, sizeof(*p));
    if (n > REG_PROXY_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        p->slot[i].id   = ids[i];
        p->slot[i].type = types[i];
    }
    p->n       = n;
    p->link_ms = now_ms;
    return find(p, REG_ID_C6_REG_CAPS) != NULL;
}

bool reg_proxy_caps_known(reg_proxy_t const *p) {
    reg_proxy_slot_t const *c = reg_proxy_slot(p, REG_ID_C6_REG_CAPS);
    return c && c->have && !c->stale;
}

// Asked and not answered within the timeout: a C6 without the registry
static bool caps_absent(reg_proxy_t const *p, uint32_t now_ms) {
    reg_proxy_slot_t const *c = reg_proxy_slot(p, REG_ID_C6_REG_CAPS);
    return c && !reg_proxy_caps_known(p) && c->pending && now_ms - c->sent_ms >= REG_PROXY_CAPS_TIMEOUT_MS;
}

bool reg_proxy_caps_due(reg_proxy_t const *p, uint32_t now_ms) {
    reg_proxy_slot_t const *c = reg_proxy_slot(p, REG_ID_C6_REG_CAPS);
    bool asking = c && c->pending && now_ms - c->sent_ms < REG_PROXY_CAPS_TIMEOUT_MS;
    return c && !reg_proxy_caps_known(p) && !asking && p->caps_tries < REG_PROXY_CAPS_TRIES;
}

rp_support_t reg_proxy_support(reg_proxy_t const *p, uint16_t id, uint32_t now_ms) {
    if (id == REG_ID_C6_REG_CAPS) {
        return RP_OK;
    }
    if (reg_proxy_caps_known(p)) {
        reg_proxy_slot_t const *c    = reg_proxy_slot(p, REG_ID_C6_REG_CAPS);
        uint32_t                bits = c->status == REG_OK ? c->value : 0;
        return bits & REG_CAP(REG_ID_DOMAIN(id)) ? RP_OK : RP_NO_DOMAIN;
    }
    return caps_absent(p, now_ms) ? RP_OLD_C6 : RP_WAIT_CAPS;
}

bool reg_proxy_begin(reg_proxy_t *p, uint16_t id, uint32_t now_ms, uint8_t *seq) {
    reg_proxy_slot_t *s = find(p, id);
    if (!s) {
        return false;
    }
    s->prev_seq     = s->seq;
    s->prev_pending = s->pending;
    s->prev_sent_ms = s->sent_ms;
    s->seq          = ++p->next_seq;
    s->pending      = true;
    s->sent_ms      = now_ms;
    if (id == REG_ID_C6_REG_CAPS && p->caps_tries < 255) {
        p->caps_tries++;
    }
    *seq = s->seq;
    return true;
}

void reg_proxy_cancel(reg_proxy_t *p, uint16_t id, uint8_t seq) {
    reg_proxy_slot_t *s = find(p, id);
    if (!s || s->seq != seq) {
        return;
    }
    s->seq     = s->prev_seq;
    s->pending = s->prev_pending;
    s->sent_ms = s->prev_sent_ms;
    if (id == REG_ID_C6_REG_CAPS && p->caps_tries) {
        p->caps_tries--;
    }
}

bool reg_proxy_pending(reg_proxy_t const *p, uint16_t id) {
    reg_proxy_slot_t const *s = reg_proxy_slot(p, id);
    return s && s->pending;
}

rp_answer_t reg_proxy_answer(reg_proxy_t *p, uint16_t id, uint8_t seq, uint8_t status, uint8_t type, uint32_t value,
                             uint32_t now_ms) {
    reg_proxy_slot_t *s = find(p, id);
    if (!s) {
        return RP_NOT_OURS;
    }
    if (!s->pending || seq != s->seq) {
        p->late++;
        return RP_LATE;
    }
    // A type this build does not know (a newer C6), or not the P4 table's: never formatted as if it were
    if (status == REG_OK && (!reg_type_known(type) || type != s->type)) {
        p->bad_type++;
        return RP_BAD_TYPE;
    }
    s->have    = true;
    s->pending = false;
    s->stale   = false;
    s->status  = status;
    s->value   = status == REG_OK ? value : 0;
    s->at_ms   = now_ms;
    return RP_ACCEPTED;
}

void reg_proxy_link(reg_proxy_t *p, uint32_t now_ms) {
    for (size_t i = 0; i < p->n; i++) {
        reg_proxy_slot_t *s = &p->slot[i];
        s->stale            = s->have;
        s->pending          = false;
        s->req_since_ms     = now_ms;
        if (s->id == REG_ID_C6_REG_CAPS) {
            s->have = false; // asked anew: the C6 may have been updated meanwhile
        }
    }
    p->caps_tries = 0;
    p->link_ms    = now_ms;
}

bool reg_proxy_mismatch(reg_proxy_t *p, uint16_t id, reg_value_t const *req, uint32_t now_ms) {
    reg_proxy_slot_t *s         = find(p, id);
    uint32_t          requested = req->u;
    if (!s) {
        return false;
    }
    if (!s->req_known || s->req_value != requested) {
        s->req_known    = true;
        s->req_value    = requested;
        s->req_since_ms = now_ms;
    }
    bool settled = s->have && s->at_ms - s->req_since_ms >= REG_PROXY_SETTLE_MS;
    bool differs = s->have && !s->stale && s->status == REG_OK &&
                   reg_applied_matches(id, req, &(reg_value_t){.u = s->value}) == RM_DIFFERS;
    if (s->warned || !settled || !differs) {
        return false;
    }
    s->warned = true;
    return true;
}

// The note of a key with nothing applied to show
static rv_note_t missing_note(reg_proxy_slot_t const *s, rp_support_t sup, bool live, bool no_link) {
    if (no_link) {
        return RV_NO_LINK;
    }
    if (sup == RP_WAIT_CAPS) {
        return RV_WAITING;
    }
    return s->pending && !live ? RV_WAITING : RV_NO_ANSWER;
}

void reg_proxy_view(reg_proxy_t const *p, uint16_t id, uint32_t now_ms, bool live, bool no_link, reg_view_t *v) {
    reg_proxy_slot_t const *s   = reg_proxy_slot(p, id);
    rp_support_t            sup = reg_proxy_support(p, id, now_ms);
    v->has_applied              = false;
    v->has_age                  = false;
    if (!s || sup == RP_OLD_C6 || (id == REG_ID_C6_REG_CAPS && caps_absent(p, now_ms))) {
        v->note = s ? RV_OLD_C6 : RV_FAILED;
        return;
    }
    if (sup == RP_NO_DOMAIN) {
        v->note = RV_NO_DOMAIN;
        return;
    }
    if (!s->have) {
        v->note = missing_note(s, sup, live, no_link);
        return;
    }
    if (s->status != REG_OK) {
        // The C6's lora.* answer FAILED until a radio start succeeded (#309): not an error, nothing applied yet
        bool radio = REG_ID_DOMAIN(id) == REG_ID_DOMAIN(REG_ID_LORA_FREQ) && s->status == REG_FAILED;
        v->note    = s->status == REG_UNKNOWN ? RV_UNKNOWN_ID : radio ? RV_NOT_APPLIED : RV_FAILED;
        return;
    }
    v->has_applied = true;
    v->applied.u   = s->value;
    v->has_age     = true;
    v->age_ms      = now_ms - s->at_ms;
    v->note        = s->stale || no_link || (live && s->pending) ? RV_STALE : RV_NONE;
}
