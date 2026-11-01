// The companion pipe and BadgeMesh (#45, #55): see mesh_link.h
#include "mesh_link.h"

#include "channel_input.h"

#include <string.h>

static void fill_hash(mesh::Packet *pkt, uint8_t hash[MAX_HASH_SIZE]) {
    pkt->calculatePacketHash(hash);
}

static bool ours_to_track(mesh::Packet *pkt, bool *is_dm) {
    uint8_t type = pkt->getPayloadType();
    *is_dm       = type == PAYLOAD_TYPE_TXT_MSG;
    return type == PAYLOAD_TYPE_GRP_TXT || *is_dm;
}

void BadgeMesh::logRx(mesh::Packet *packet, int len, float score) {
    if (packet->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) {
        return;
    }
    uint8_t hash[MAX_HASH_SIZE];
    fill_hash(packet, hash);
    mesh_ob_enter();
    ob_on_rx(ob_, hash); // a repeat of our own channel message, before Mesh drops it as a duplicate
    mesh_ob_exit();
}

void BadgeMesh::logTx(mesh::Packet *packet, int len) {
    bool is_dm;
    if (!ours_to_track(packet, &is_dm)) {
        return;
    }
    uint8_t hash[MAX_HASH_SIZE];
    fill_hash(packet, hash);
    mesh_ob_enter();
    ob_on_tx(ob_, is_dm, hash);
    mesh_ob_exit();
}

void BadgeMesh::logTxFail(mesh::Packet *packet, int len) {
    bool is_dm;
    if (!ours_to_track(packet, &is_dm)) {
        return;
    }
    mesh_ob_enter();
    ob_on_tx_fail(ob_, is_dm, (uint32_t)millis());
    mesh_ob_exit();
}

static void contact_to_frame(ContactInfo const &c, mf_contact_t *out) {
    memcpy(out->pub, c.id.pub_key, MF_PUB_LEN);
    out->type         = c.type;
    out->flags        = c.flags;
    out->out_path_len = c.out_path_len;
    memcpy(out->out_path, c.out_path, MF_PATH_MAX);
    memcpy(out->name, c.name, MF_NAME_MAX);
    out->last_advert_timestamp = c.last_advert_timestamp;
    out->lastmod               = c.lastmod;
    out->gps_lat               = c.gps_lat;
    out->gps_lon               = c.gps_lon;
}

BadgeMesh::ContactResult BadgeMesh::contactAction(uint8_t const prefix[MF_PREFIX], mesh_contact_op_t op) {
    ContactInfo *c = lookupContactByPubKey(prefix, MF_PREFIX);
    if (!c) {
        return CONTACT_GONE;
    }
    if (!pipe_.can_queue()) {
        return CONTACT_QUEUE_FULL; // checked first: nothing changes that MyMesh would then not save
    }
    uint8_t      f[MF_MAX_FRAME];
    size_t       n = 0;
    mf_contact_t mc;
    switch (op) {
    case MESH_CO_RESET_PATH:
        resetPathTo(*c);
        n = mf_encode_reset_path(f, c->id.pub_key);
        break;
    case MESH_CO_FAV_ON:
    case MESH_CO_FAV_OFF:
        c->flags = (uint8_t)(op == MESH_CO_FAV_ON ? (c->flags | MESH_CONTACT_FAV) : (c->flags & ~MESH_CONTACT_FAV));
        contact_to_frame(*c, &mc);
        n = mf_encode_update_contact(f, &mc);
        break;
    case MESH_CO_REMOVE: n = mf_encode_remove_contact(f, c->id.pub_key); break;
    }
    return n && pipe_.queue_cmd(f, n) ? CONTACT_QUEUED : CONTACT_QUEUE_FULL;
}

// ---- the pipe ------------------------------------------------------------------------------------------------------

bool CompanionPipe::fifo_push(uint8_t const *data, size_t len) {
    if (fifo_n_ >= LINK_FIFO || len > MF_MAX_FRAME) {
        stats_.fifo_drops++;
        return false;
    }
    Frame &f = fifo_[(fifo_head_ + fifo_n_) % LINK_FIFO];
    f.len    = (uint8_t)len;
    memcpy(f.data, data, len);
    fifo_n_++;
    return true;
}

void CompanionPipe::request_drain() {
    if (draining_) {
        return; // the running drain ends at NO_MORE_MESSAGES and covers what arrived meanwhile
    }
    if (fifo_n_ >= LINK_FIFO) {
        stats_.fifo_drops++;
        return;
    }
    uint8_t f[1];
    draining_ = fifo_push(f, mf_encode_sync_next(f));
}

void CompanionPipe::start() {
    uint8_t f[MF_MAX_FRAME];
    fifo_push(f, mf_encode_device_query(f, 3));
    fifo_push(f, mf_encode_app_start(f, "lichen"));
    request_drain(); // the queue is empty at boot; this checks the loop end to end
}

// The channel slot by name: the slots can change between compose and send, so the UI never holds an index
static int channel_slot(MyMesh *mesh, char const *name) {
    ChannelDetails d;
    for (int i = 0; i < MAX_GROUP_CHANNELS && mesh->getChannel(i, d); i++) {
        if (d.name[0] && channel_same_name(d.name, name)) {
            return i;
        }
    }
    return -1;
}

size_t CompanionPipe::build_outbox_frame(ob_action_t const &act, uint8_t *dest) {
    size_t text_len = strlen(act.text);
    if (act.kind == OB_ACT_SEND_CHANNEL) {
        int slot = channel_slot(mesh_, act.target);
        if (slot < 0) {
            return 0;
        }
        size_t room = mf_channel_text_max(strlen(mesh_->getNodeName()));
        text_len    = mf_text_cut(act.text, room);
        return text_len ? mf_encode_send_channel(dest, (uint8_t)slot, act.ts, act.text, text_len) : 0;
    }
    ContactInfo *c = mesh_->lookupContactByPubKey(act.prefix, MF_PREFIX);
    if (!c) {
        return 0;
    }
    if (act.kind == OB_ACT_RESET_PATH) {
        return mf_encode_reset_path(dest, c->id.pub_key);
    }
    return mf_encode_send_dm(dest, act.attempt, act.ts, act.prefix, act.text, mf_text_cut(act.text, MF_MAX_TEXT));
}

size_t CompanionPipe::checkRecvFrame(uint8_t dest[]) {
    if (cur_ != CUR_NONE) {
        // The last command got no reply (MyMesh ignores a frame it does not understand): do not wait for it
        stats_.lost_replies++;
        if (cur_ == CUR_OUTBOX) {
            mesh_ob_enter();
            ob_fail(ob_, cur_id_);
            mesh_ob_exit();
        }
        cur_ = CUR_NONE;
    }
    bool fifo_has = fifo_n_ > 0;
    bool ob_first = ob_arb_outbox_first(&arb_, fifo_has);
    size_t n      = ob_first ? take_outbox(dest) : 0;
    if (n == 0 && fifo_has && fifo_n_ > 0) {
        n = take_fifo(dest);
    }
    return n;
}

size_t CompanionPipe::take_fifo(uint8_t dest[]) {
    Frame &f = fifo_[fifo_head_];
    size_t n = f.len;
    memcpy(dest, f.data, n);
    fifo_head_ = (fifo_head_ + 1) % LINK_FIFO;
    fifo_n_--;
    cur_     = CUR_INTERNAL;
    cur_cmd_ = dest[0];
    stats_.cmds_out++;
    ob_arb_took(&arb_, true);
    return n;
}

size_t CompanionPipe::take_outbox(uint8_t dest[]) {
    mesh_ob_enter();
    bool have = ob_next_action(ob_, (uint32_t)millis(), 0, &act_);
    mesh_ob_exit();
    if (!have) {
        return 0;
    }
    if (act_.ts == 0) {
        act_.ts = rtc_clock.getCurrentTimeUnique(); // only here: every call of it moves MeshCore's clock on
        mesh_ob_enter();
        ob_set_ts(ob_, act_.id, act_.ts);
        mesh_ob_exit();
    }
    size_t n = build_outbox_frame(act_, dest);
    if (n == 0) { // unknown channel or contact, or nothing left after the cut
        mesh_ob_enter();
        ob_fail(ob_, act_.id);
        mesh_ob_exit();
        return 0;
    }
    cur_     = CUR_OUTBOX;
    cur_cmd_ = dest[0];
    cur_id_  = act_.id;
    stats_.cmds_out++;
    ob_arb_took(&arb_, false);
    return n;
}

void CompanionPipe::on_reply(mf_msg_t const &m) {
    if (m.code < 32) {
        stats_.reply_count[m.code]++;
    }
    CurKind cur = cur_;
    cur_        = CUR_NONE;
    if (cur == CUR_OUTBOX) {
        mesh_ob_enter();
        ob_on_reply(ob_, cur_id_, &m, (uint32_t)millis());
        mesh_ob_exit();
        return;
    }
    switch (m.kind) {
    case MF_MSG_RECV: // thrown away: the Listener already fed the screens with this message
        stats_.drained++;
        {
            uint8_t f[1];
            if (!fifo_push(f, mf_encode_sync_next(f))) {
                draining_ = false; // the loop would hang otherwise: the next MSG_WAITING restarts it
            }
        }
        break;
    case MF_NO_MORE:
        stats_.no_more++;
        draining_ = false;
        break;
    case MF_DEVICE_INFO:
        stats_.app_ver = m.fw_ver;
        break;
    case MF_SELF_INFO: {
        bool same = mesh_ && memcmp(m.pub, mesh_->self_id.pub_key, MF_PUB_LEN) == 0;
        (same ? stats_.self_ok : stats_.self_bad)++;
        break;
    }
    default:
        break;
    }
    if (cur == CUR_INTERNAL && cur_cmd_ == MF_CMD_SYNC_NEXT && m.kind != MF_MSG_RECV && m.kind != MF_NO_MORE) {
        draining_ = false; // an answer we do not know: stop draining, the next MSG_WAITING starts again
    }
}

void CompanionPipe::on_push(mf_msg_t const &m) {
    stats_.pushes++;
    if (m.code >= 0x80 && m.code - 0x80 < LINK_PUSH_KINDS) {
        stats_.push_count[m.code - 0x80]++;
    }
    switch (m.kind) {
    case MF_MSG_WAITING:
        stats_.msg_waiting++;
        request_drain();
        break;
    case MF_SEND_CONFIRMED:
        stats_.confirmed++;
        mesh_ob_enter();
        ob_on_confirm(ob_, m.ack, m.trip_ms);
        mesh_ob_exit();
        break;
    default:
        break; // adverts, path updates, login/status replies: counted, #47 and later issues use them
    }
}

size_t CompanionPipe::writeFrame(uint8_t const src[], size_t len) {
    mf_decode(src, len, &scratch_);
    if (scratch_.kind == MF_BAD) {
        stats_.bad_frames++;
    }
    if (len > 0 && src[0] >= 0x80) {
        on_push(scratch_);
    } else {
        stats_.replies++;
        on_reply(scratch_);
    }
    return len;
}
