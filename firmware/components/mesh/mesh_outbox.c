#include "mesh_outbox.h"

#include <string.h>

static bool due(uint32_t now, uint32_t at) {
    return (int32_t)(now - at) >= 0;
}

void ob_init(ob_t *ob) {
    memset(ob, 0, sizeof(*ob));
}

static ob_entry_t *find(ob_t *ob, mesh_msg_id_t id) {
    for (int i = 0; id && i < OB_CAP; i++) {
        if (ob->e[i].used && ob->e[i].id == id) {
            return &ob->e[i];
        }
    }
    return NULL;
}

void ob_set_ts(ob_t *ob, mesh_msg_id_t id, uint32_t ts) {
    ob_entry_t *e = find(ob, id);
    if (e && e->ts == 0) {
        e->ts = ts;
    }
}

static bool dm_active(ob_entry_t const *e) {
    return e->used && e->kind == OB_DM &&
           (e->awaiting || e->state == MESH_OUT_QUEUED || e->state == MESH_OUT_SENT || e->state == MESH_OUT_ON_AIR);
}

// Finished with: nothing more will happen that the table must remember
static bool evictable(ob_entry_t const *e) {
    return !e->used || (!e->awaiting && e->state != MESH_OUT_QUEUED && !dm_active(e));
}

static ob_entry_t *take_slot(ob_t *ob) {
    ob_entry_t *best = NULL;
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (!e->used) {
            return e;
        }
        if (evictable(e) && (!best || (int32_t)(e->seq - best->seq) < 0)) {
            best = e;
        }
    }
    return best;
}

static mesh_msg_id_t new_id(ob_t *ob) {
    ob->next_id++;
    if (ob->next_id == 0) {
        ob->next_id = 1; // 0 means "not queued"
    }
    return ob->next_id;
}

static mesh_msg_id_t add(ob_t *ob, ob_kind_t kind, char const *text, size_t text_len, uint32_t now_ms,
                         char const **why) {
    if (text_len == 0 || text_len > MF_MAX_TEXT) {
        *why = text_len ? "too long" : "empty";
        return 0;
    }
    ob_entry_t *e = take_slot(ob);
    if (!e) {
        *why = "busy";
        return 0;
    }
    memset(e, 0, sizeof(*e));
    e->used          = true;
    e->kind          = (uint8_t)kind;
    e->state         = MESH_OUT_QUEUED;
    e->id            = new_id(ob);
    e->seq           = ob->next_seq++;
    e->not_before_ms = now_ms;
    memcpy(e->text, text, text_len);
    return e->id;
}

mesh_msg_id_t ob_add_channel(ob_t *ob, char const *channel, char const *text, size_t text_len, uint32_t now_ms,
                             char const **why) {
    mesh_msg_id_t id = add(ob, OB_CHANNEL, text, text_len, now_ms, why);
    if (id) {
        strncpy(find(ob, id)->target, channel, OB_TARGET_MAX - 1);
    }
    return id;
}

static bool dm_allowed(ob_t *ob, uint8_t const prefix[MF_PREFIX]) {
    int active = 0, acks = 0;
    for (int i = 0; i < OB_CAP; i++) {
        if (dm_active(&ob->e[i])) {
            active++;
            acks += OB_MAX_ATTEMPTS; // worst case: one ACK per attempt
            if (memcmp(ob->e[i].prefix, prefix, MF_PREFIX) == 0) {
                return false; // one at a time per contact
            }
        }
    }
    return active < OB_MAX_DM_INFLIGHT && acks + OB_MAX_ATTEMPTS < OB_ACK_SLOTS;
}

mesh_msg_id_t ob_add_dm(ob_t *ob, uint8_t const prefix[MF_PREFIX], char const *text, size_t text_len, uint32_t now_ms,
                        char const **why) {
    if (!dm_allowed(ob, prefix)) {
        *why = "busy";
        return 0;
    }
    mesh_msg_id_t id = add(ob, OB_DM, text, text_len, now_ms, why);
    if (id) {
        memcpy(find(ob, id)->prefix, prefix, MF_PREFIX);
    }
    return id;
}

// A DM whose ACK has not come by its deadline: another attempt, or failed
static void expire(ob_entry_t *e, uint32_t now) {
    if (e->kind != OB_DM || e->awaiting || (e->state != MESH_OUT_SENT && e->state != MESH_OUT_ON_AIR) ||
        !due(now, e->deadline_ms)) {
        return;
    }
    if (e->attempt >= OB_MAX_ATTEMPTS) {
        e->state = MESH_OUT_FAILED;
        return;
    }
    e->state         = MESH_OUT_QUEUED;
    e->not_before_ms = now;
    // After two direct tries the path is probably stale: forget it, so the last try floods
    e->pending_reset = e->attempt == OB_MAX_ATTEMPTS - 1 && !e->flood_seen;
}

static void fill_action(ob_entry_t *e, uint32_t ts_now, ob_action_t *act) {
    memset(act, 0, sizeof(*act));
    act->id = e->id;
    memcpy(act->prefix, e->prefix, MF_PREFIX);
    memcpy(act->target, e->target, sizeof(act->target));
    memcpy(act->text, e->text, sizeof(act->text));
    if (e->ts == 0) {
        e->ts = ts_now;
    }
    act->ts        = e->ts;
    e->awaiting    = true;
    e->have_hash   = false;
    if (e->pending_reset) {
        act->kind              = OB_ACT_RESET_PATH;
        e->pending_reset       = false;
        e->awaiting_reset      = true;
    } else if (e->kind == OB_CHANNEL) {
        act->kind = OB_ACT_SEND_CHANNEL;
    } else {
        act->kind    = OB_ACT_SEND_DM;
        act->attempt = e->attempt; // 0-based on the wire
        e->attempt++;
    }
}

bool ob_next_action(ob_t *ob, uint32_t now_ms, uint32_t ts_now, ob_action_t *act) {
    ob_entry_t *pick = NULL;
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (!e->used) {
            continue;
        }
        expire(e, now_ms);
        if (e->state == MESH_OUT_QUEUED && !e->awaiting && due(now_ms, e->not_before_ms) &&
            (!pick || (int32_t)(e->seq - pick->seq) < 0)) {
            pick = e; // oldest first
        }
    }
    if (!pick) {
        act->kind = OB_ACT_NONE;
        return false;
    }
    fill_action(pick, ts_now, act);
    return true;
}

static void dm_attempt_failed(ob_entry_t *e, uint32_t now) {
    if (e->attempt >= OB_MAX_ATTEMPTS) {
        e->state = MESH_OUT_FAILED;
    } else {
        e->state         = MESH_OUT_QUEUED;
        e->not_before_ms = now + OB_RETRY_BACKOFF_MS;
    }
}

static void dm_sent(ob_entry_t *e, mf_msg_t const *m, uint32_t now) {
    if (m->ack && e->n_acks < OB_MAX_ATTEMPTS) {
        e->acks[e->n_acks++] = m->ack;
    }
    e->flood_seen = e->flood_seen || m->flood;
    e->state      = MESH_OUT_SENT;
    e->sent_ms    = now;
    uint32_t wait = m->est_timeout + m->est_timeout / 4;
    e->deadline_ms = now + (wait < OB_TIMEOUT_FLOOR_MS ? OB_TIMEOUT_FLOOR_MS : wait);
}

void ob_on_reply(ob_t *ob, mesh_msg_id_t id, mf_msg_t const *m, uint32_t now_ms) {
    ob_entry_t *e = find(ob, id);
    if (!e || !e->awaiting) {
        return;
    }
    e->awaiting = false;
    if (e->awaiting_reset) {
        e->awaiting_reset = false; // the next action is the send; a refused reset changes nothing
        return;
    }
    if (e->kind == OB_CHANNEL) {
        e->state   = m->kind == MF_OK ? MESH_OUT_SENT : MESH_OUT_FAILED;
        e->sent_ms = now_ms;
    } else if (m->kind == MF_SENT) {
        dm_sent(e, m, now_ms);
    } else if (m->kind == MF_ERR && m->err == MF_ERR_NOT_FOUND) {
        e->state = MESH_OUT_FAILED; // the contact is gone
    } else {
        dm_attempt_failed(e, now_ms); // table full, or anything unexpected
    }
}

void ob_fail(ob_t *ob, mesh_msg_id_t id) {
    ob_entry_t *e = find(ob, id);
    if (e) {
        e->state          = MESH_OUT_FAILED;
        e->awaiting       = false;
        e->awaiting_reset = false;
    }
}

size_t ob_fail_pending(ob_t *ob) {
    size_t n = 0;
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (!e->used || e->state == MESH_OUT_FAILED || e->state == MESH_OUT_DELIVERED) {
            continue;
        }
        bool waiting = e->state == MESH_OUT_QUEUED || e->state == MESH_OUT_SENT;
        if (e->kind == OB_DM || waiting) {
            e->state          = MESH_OUT_FAILED;
            e->awaiting       = false;
            e->awaiting_reset = false;
            e->pending_reset  = false;
            n++;
        }
    }
    return n;
}

// The oldest message of that kind that MyMesh took but whose packet we have not seen leave yet
static ob_entry_t *awaiting_air(ob_t *ob, bool dm) {
    ob_entry_t *best = NULL;
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (e->used && (e->kind == OB_DM) == dm && e->state == MESH_OUT_SENT && !e->have_hash && !e->awaiting &&
            (!best || (int32_t)(e->seq - best->seq) < 0)) {
            best = e;
        }
    }
    return best;
}

void ob_on_tx(ob_t *ob, bool payload_is_dm, uint8_t const hash[OB_HASH_LEN]) {
    ob_entry_t *e = awaiting_air(ob, payload_is_dm);
    if (e) {
        memcpy(e->hash, hash, OB_HASH_LEN);
        e->have_hash = true;
        e->state     = MESH_OUT_ON_AIR;
    }
}

void ob_on_tx_fail(ob_t *ob, bool payload_is_dm, uint32_t now_ms) {
    ob_entry_t *e = awaiting_air(ob, payload_is_dm);
    if (!e) {
        return;
    }
    if (e->kind == OB_CHANNEL) {
        e->state = MESH_OUT_FAILED;
    } else {
        e->deadline_ms = now_ms + OB_RETRY_BACKOFF_MS; // the attempt is lost: retry after a pause, not at once
    }
}

void ob_on_rx(ob_t *ob, uint8_t const hash[OB_HASH_LEN]) {
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (e->used && e->kind == OB_CHANNEL && e->have_hash && (e->state == MESH_OUT_ON_AIR || e->state == MESH_OUT_HEARD) &&
            memcmp(e->hash, hash, OB_HASH_LEN) == 0) {
            if (e->repeats < 255) {
                e->repeats++;
            }
            e->state = MESH_OUT_HEARD;
        }
    }
}

void ob_on_confirm(ob_t *ob, uint32_t ack, uint32_t trip_ms) {
    for (int i = 0; ack && i < OB_CAP; i++) {
        ob_entry_t *e = &ob->e[i];
        if (!e->used || e->kind != OB_DM || e->state == MESH_OUT_DELIVERED) {
            continue; // a duplicate ACK changes nothing
        }
        for (int k = 0; k < e->n_acks; k++) {
            if (e->acks[k] == ack) { // an earlier attempt's ACK counts too
                e->state   = MESH_OUT_DELIVERED;
                e->trip_ms = trip_ms;
                return;
            }
        }
    }
}

bool ob_retry(ob_t *ob, mesh_msg_id_t id, uint32_t now_ms) {
    ob_entry_t *e = find(ob, id);
    if (!e || e->state != MESH_OUT_FAILED || e->awaiting) {
        return false;
    }
    if (e->kind == OB_DM) {
        e->state = MESH_OUT_DELIVERED; // out of the way for the limit check
        bool ok  = dm_allowed(ob, e->prefix);
        e->state = MESH_OUT_FAILED;
        if (!ok) {
            return false;
        }
    }
    e->state         = MESH_OUT_QUEUED;
    e->attempt       = 0;
    e->n_acks        = 0;
    e->ts            = 0;
    e->flood_seen    = false;
    e->pending_reset = false;
    e->have_hash     = false;
    e->repeats       = 0;
    e->not_before_ms = now_ms;
    return true;
}

bool ob_get(ob_t const *ob, mesh_msg_id_t id, mesh_out_state_t *st, uint8_t *attempt, uint8_t *repeats,
            uint32_t *trip_ms) {
    for (int i = 0; id && i < OB_CAP; i++) {
        ob_entry_t const *e = &ob->e[i];
        if (e->used && e->id == id) {
            if (st) {
                *st = (mesh_out_state_t)e->state;
            }
            if (attempt) {
                *attempt = e->attempt;
            }
            if (repeats) {
                *repeats = e->repeats;
            }
            if (trip_ms) {
                *trip_ms = e->trip_ms;
            }
            return true;
        }
    }
    return false;
}

bool ob_arb_outbox_first(ob_arb_t const *arb, bool fifo_has) {
    return !fifo_has || arb->last_internal;
}

void ob_arb_took(ob_arb_t *arb, bool internal) {
    arb->last_internal = internal;
}
