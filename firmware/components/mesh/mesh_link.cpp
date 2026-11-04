// The companion pipe and BadgeMesh (#45, #55): see mesh_link.h
#include "mesh_link.h"

#include "channel_input.h"
#include "mesh_wipe.h"

#include <math.h>
#include <stdio.h>
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
    // Every packet that parsed, headers only (#165): before the filter below, which is about our own channel messages
    mesh_airlog_note(packet->header, (uint8_t)packet->path_len, packet->path, (uint8_t)len, _radio->getLastRSSI(),
                     _radio->getLastSNR());
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
    mt_t *mt = pipe_.trace();
    if (packet->getPayloadType() == PAYLOAD_TYPE_TRACE && mt && packet->payload_len >= 4) {
        uint32_t tag;
        memcpy(&tag, packet->payload, 4);
        mesh_ob_enter();
        mt_on_tx(mt, tag, (uint32_t)millis()); // our trace on air: its round trip counts from here (#53)
        mesh_ob_exit();
        return;
    }
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

void BadgeMesh::onAdvertRecv(mesh::Packet *packet, mesh::Identity const &id, uint32_t timestamp,
                             uint8_t const *app_data, size_t app_data_len) {
    if (lookupContactByPubKey(id.pub_key, PUB_KEY_SIZE)) {
        // Only a known contact has a history (#166); the advert's path is what MeshCore keeps for it too
        mesh_signal_note(id.pub_key, true, packet->path_len, packet->path, _radio->getLastRSSI(), _radio->getLastSNR());
    } else {
        advert_limit_result_t r = advert_limit_take(&advert_limit_, (uint32_t)millis());
        if (r != ADVERT_LIMIT_OK) {
            if (r == ADVERT_LIMIT_REFUSED_FIRST) {
                mesh_advert_limited(advert_limit_.refused);
            }
            return;
        }
    }
    MyMesh::onAdvertRecv(packet, id, timestamp, app_data, app_data_len);
}

void BadgeMesh::onMessageRecv(ContactInfo const &from, mesh::Packet *packet, uint32_t sender_timestamp,
                              char const *text) {
    // the hop count it came over (review #166: was always 0 = "direct"); the path itself is kept for adverts only
    mesh_signal_note(from.id.pub_key, false, packet->path_len, NULL, _radio->getLastRSSI(), _radio->getLastSNR());
    MyMesh::onMessageRecv(from, packet, sender_timestamp, text);
}

// A room's post: the contact table names the author by the first 4 bytes of its key (the room sends only those), else
// the 8 hex digits. The screens see "<author>: <text>" in the room's chat (#51).
void BadgeMesh::onSignedMessageRecv(ContactInfo const &from, mesh::Packet *packet, uint32_t sender_timestamp,
                                    uint8_t const *sender_prefix, char const *text) {
    char who[MF_NAME_MAX + 1];
    ContactInfo *a = lookupContactByPubKey(sender_prefix, 4);
    if (a && a->name[0]) {
        snprintf(who, sizeof(who), "%s", a->name);
    } else {
        snprintf(who, sizeof(who), "%02x%02x%02x%02x", sender_prefix[0], sender_prefix[1], sender_prefix[2],
                 sender_prefix[3]);
    }
    char   line[MF_MAX_TEXT + 1];
    size_t room = MF_MAX_TEXT - strlen(who) - 2;
    snprintf(line, sizeof(line), "%s: %.*s", who, (int)mf_text_cut(text, room), text);
    MyMesh::onSignedMessageRecv(from, packet, sender_timestamp, sender_prefix, line);
}

bool BadgeMesh::remoteLogout(uint8_t const prefix[MF_PREFIX]) {
    ContactInfo *c = lookupContactByPubKey(prefix, MF_PREFIX);
    if (!c || !pipe_.can_queue()) {
        return false;
    }
    uint8_t f[MF_MAX_FRAME];
    return pipe_.queue_cmd(f, mf_encode_logout(f, c->id.pub_key));
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

bool BadgeMesh::advertNow(bool flood) {
    if (!flood) {
        return advert(); // MyMesh's zero-hop advert
    }
    NodePrefs const *prefs = getNodePrefs();
    mesh::Packet    *pkt   = prefs->advert_loc_policy == ADVERT_LOC_NONE
                                 ? createSelfAdvert(prefs->node_name)
                                 : createSelfAdvert(prefs->node_name, sensors.node_lat, sensors.node_lon);
    if (!pkt) {
        return false;
    }
    TransportKey scope; // the default scope, as CMD_SEND_SELF_ADVERT sends it (all zero = unscoped)
    memcpy(&scope.key, prefs->default_scope_key, sizeof(scope.key));
    sendFloodScoped(scope, pkt, 0);
    return true;
}

int BadgeMesh::dropOutbound() {
    int n = 0;
    while (_mgr->getOutboundTotal() > 0) {
        mesh::Packet *p = _mgr->removeOutboundByIdx(0);
        if (!p) {
            break;
        }
        _mgr->free(p);
        n++;
    }
    return n;
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
        // The frame is a snapshot of the whole contact that MyMesh applies in its next loop (ms later): a path learned
        // in between would be overwritten by the old one. Accepted: the window is one loop, and the next DM or
        // advert learns the path again.
        contact_to_frame(*c, &mc);
        n = mf_encode_update_contact(f, &mc);
        break;
    case MESH_CO_REMOVE: n = mf_encode_remove_contact(f, c->id.pub_key); break;
    }
    return n && pipe_.queue_cmd(f, n) ? CONTACT_QUEUED : CONTACT_QUEUE_FULL;
}

// The live entry for a copy from the iterator when its route is stale
static ContactInfo *mesh_contact_is_stale_ptr(BadgeMesh *m, ContactInfo const &copy, uint8_t hash_size) {
    return mesh_path_stale(copy.out_path_len, hash_size) ? m->lookupContactByPubKey(copy.id.pub_key, PUB_KEY_SIZE)
                                                         : nullptr;
}

int BadgeMesh::forgetStaleRoutes(uint8_t hash_size) {
    int          n = 0;
    ContactInfo  copy;
    ContactsIterator it = startContactsIterator();
    while (it.hasNext(this, copy)) {
        ContactInfo *c = mesh_contact_is_stale_ptr(this, copy, hash_size);
        if (c) {
            resetPathTo(*c);
            n++;
        }
    }
    return n;
}

// A manual path (#130): set at once, then the whole contact goes to MyMesh as CMD_ADD_UPDATE_CONTACT so it saves it (the
// same way as store_discovered_path). lastmod stays: setting a path is not hearing the contact.
BadgeMesh::ContactResult BadgeMesh::contactSetPath(uint8_t const prefix[MF_PREFIX], uint8_t path_len,
                                                   uint8_t const *path) {
    ContactInfo *c = lookupContactByPubKey(prefix, MF_PREFIX);
    if (!c) {
        return CONTACT_GONE;
    }
    if (!pipe_.can_queue()) {
        return CONTACT_QUEUE_FULL;
    }
    c->out_path_len = path_len;
    memcpy(c->out_path, path, sizeof(c->out_path));
    uint8_t      f[MF_MAX_FRAME];
    mf_contact_t mc;
    contact_to_frame(*c, &mc);
    size_t n = mf_encode_update_contact(f, &mc);
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

// A command of the badge's own: into the FIFO, or when that is full into the overflow, which feeds the FIFO as it
// empties (a favourite toggle must not be lost to a busy queue, #112)
bool CompanionPipe::queue_cmd(uint8_t const *frame, size_t len) {
    if (fifo_n_ < LINK_FIFO && ovf_n_ == 0) {
        return fifo_push(frame, len); // never past commands already waiting in the overflow: they keep their order
    }
    if (ovf_n_ >= LINK_OVF || len > MF_MAX_FRAME) {
        return false;
    }
    Frame &f = ovf_[(ovf_head_ + ovf_n_) % LINK_OVF];
    f.len    = (uint8_t)len;
    memcpy(f.data, frame, len);
    ovf_n_++;
    return true;
}

void CompanionPipe::refill_from_ovf() {
    while (ovf_n_ > 0 && fifo_n_ < LINK_FIFO) {
        Frame const &f = ovf_[ovf_head_];
        if (!fifo_push(f.data, f.len)) {
            stats_.fifo_drops++; // cannot happen (room checked above); counted so it would show in fw mesh link
        }
        ovf_head_ = (ovf_head_ + 1) % LINK_OVF;
        ovf_n_--;
    }
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
    if (wipe_frame_) {
        // The last remote frame (a login or a command, maybe a password) was handled in the loop that took it: it does
        // not stay in MyMesh's command buffer (#123)
        mesh_wipe(wipe_frame_, wipe_len_);
        wipe_frame_ = nullptr;
    }
    if (cur_ != CUR_NONE) {
        // The last command got no reply (MyMesh ignores a frame it does not understand): do not wait for it
        stats_.lost_replies++;
        if (cur_ == CUR_OUTBOX) {
            mesh_ob_enter();
            ob_fail(ob_, cur_id_);
            mesh_ob_exit();
        } else if (cur_ == CUR_TRACE) {
            mesh_ob_enter();
            mt_fail(mt_, "MeshCore did not answer");
            mesh_ob_exit();
        } else if (cur_ == CUR_REMOTE) {
            mesh_ob_enter();
            rr_fail(rr_, "MeshCore did not answer");
            mesh_ob_exit();
        }
        cur_ = CUR_NONE;
    }
    if (rr_) {
        mesh_ob_enter();
        rr_poll(rr_, (uint32_t)millis(), nullptr); // an answer that never came ends the request, and frees the trace
        mesh_ob_exit();
    }
    refill_from_ovf();
    if (size_t n = take_trace(dest)) {
        return n; // asked for by hand, one at a time: ahead of the queues
    }
    if (size_t n = take_remote(dest)) {
        return n; // asked for by hand with an Enter, as the trace
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

// The trace or path discovery the UI asked for (#53), as MyMesh's own command for the contact
size_t CompanionPipe::take_trace(uint8_t dest[]) {
    if (!mt_) {
        return 0;
    }
    if (rr_) {
        // MyMesh holds one pending login/status/discovery: a login/status running, or asked before this trace, goes first
        mesh_ob_enter();
        bool login_busy = rr_busy(rr_) && rr_->op != RR_CLI &&
                          (rr_->state != RR_QUEUED || (int32_t)(mt_->asked_ms - rr_->asked_ms) >= 0);
        mesh_ob_exit();
        if (login_busy) {
            return 0;
        }
    }
    mt_kind_t kind;
    uint8_t   prefix[MF_PREFIX];
    uint32_t  tag;
    mesh_ob_enter();
    bool have = mt_take(mt_, &kind, prefix, &tag, (uint32_t)millis());
    mesh_ob_exit();
    if (!have) {
        return 0;
    }
    ContactInfo *c   = mesh_->lookupContactByPubKey(prefix, MF_PREFIX);
    char const  *why = c ? NULL : "contact not found";
    size_t       n   = 0;
    if (c && kind == MT_DISCOVER) {
        n = mf_encode_discovery(dest, c->id.pub_key);
    } else if (c) {
        uint8_t direct_size = (uint8_t)(mesh_->getNodePrefs()->path_hash_mode + 1);
        why = mt_plan(c->type, c->out_path_len, c->out_path, c->id.pub_key, direct_size, &plan_);
        if (!why) {
            n = mf_encode_trace(dest, tag, 0, plan_.flags, plan_.hashes, plan_.hash_len);
        }
    }
    mesh_ob_enter();
    if (n == 0) {
        mt_fail(mt_, why ? why : "the path does not fit in a trace");
    } else if (kind == MT_TRACE) {
        mt_set_target(mt_, plan_.target);
    }
    mesh_ob_exit();
    if (n == 0) {
        return 0;
    }
    cur_     = CUR_TRACE;
    cur_cmd_ = dest[0];
    stats_.cmds_out++;
    return n;
}

// The login, status request or CLI command the person asked for (#51), as MyMesh's own command. MyMesh keeps one
// pending login/status/discovery at a time (clearPendingReqs), so this waits while a trace or discovery is running.
size_t CompanionPipe::take_remote(uint8_t dest[]) {
    if (!rr_) {
        return 0;
    }
    if (mt_) {
        mesh_ob_enter();
        bool trace_busy = mt_busy(mt_) && (mt_->state != MT_QUEUED || (int32_t)(rr_->asked_ms - mt_->asked_ms) > 0);
        mesh_ob_exit();
        if (trace_busy) {
            return 0;
        }
    }
    rr_op_t  op;
    uint8_t  prefix[MF_PREFIX];
    char     arg[RR_ARG_MAX + 1];
    mesh_ob_enter();
    bool have = rr_take(rr_, &op, prefix, arg, sizeof(arg), (uint32_t)millis());
    mesh_ob_exit();
    if (!have) {
        return 0;
    }
    ContactInfo *c   = mesh_->lookupContactByPubKey(prefix, MF_PREFIX);
    size_t       n   = 0;
    char const  *why = c ? "the request does not fit in a frame" : "contact not found";
    if (c) {
        switch (op) {
        case RR_LOGIN: n = mf_encode_login(dest, c->id.pub_key, arg); break;
        case RR_STATUS: n = mf_encode_status_req(dest, c->id.pub_key); break;
        case RR_TELEMETRY: n = mf_encode_telemetry_req(dest, c->id.pub_key); break;
        case RR_CLI:
            n = mf_encode_send_cli(dest, 0, rtc_clock.getCurrentTimeUnique(), prefix, arg, mf_text_cut(arg, MF_MAX_TEXT));
            break;
        }
    }
    mesh_wipe(arg, sizeof(arg)); // the password is on its way (or refused): not kept on this stack
    if (n) {
        wipe_frame_ = dest; // MyMesh reads the frame after this returns: it goes at the next call, see checkRecvFrame
        wipe_len_   = n;
    }
    if (n == 0) {
        mesh_ob_enter();
        rr_fail(rr_, why);
        mesh_ob_exit();
        return 0;
    }
    cur_     = CUR_REMOTE;
    cur_cmd_ = dest[0];
    stats_.cmds_out++;
    return n;
}

// A path discovery's answer becomes the contact's stored path. MyMesh does not store it itself (its onContactPathRecv
// returns before BaseChatMesh's, as the phone app sets the path); it goes back as MyMesh's own CMD_ADD_UPDATE_CONTACT,
// so MyMesh saves it with its contacts, and is set at once so a copy taken right after shows it.
void CompanionPipe::store_discovered_path(mf_discovery_t const &d) {
    ContactInfo *c = mesh_->lookupContactByPubKey(d.prefix, MF_PREFIX);
    if (!c) {
        return; // removed meanwhile
    }
    size_t bytes = (size_t)(d.out_len & 63) * (size_t)((d.out_len >> 6) + 1); // valid: mf_decode checked it
    c->out_path_len = d.out_len;
    memset(c->out_path, 0, sizeof(c->out_path));
    memcpy(c->out_path, d.out_path, bytes);
    c->lastmod = rtc_clock.getCurrentTime(); // heard from it just now, as BaseChatMesh does for a new path
    mf_contact_t mc;
    contact_to_frame(*c, &mc);
    uint8_t f[MF_MAX_FRAME];
    size_t  n = mf_encode_update_contact(f, &mc);
    if (n && fifo_push(f, n)) { // a full queue counts a fifo drop: the path is set, saved with the next contacts write
        stats_.paths_stored++;
    }
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
    if (cur == CUR_TRACE) {
        mesh_ob_enter();
        mt_on_reply(mt_, &m, (uint32_t)millis());
        mesh_ob_exit();
        return;
    }
    if (cur == CUR_REMOTE) {
        mesh_ob_enter();
        rr_on_reply(rr_, &m, (uint32_t)millis());
        mesh_ob_exit();
        return;
    }
    switch (m.kind) {
    case MF_MSG_RECV: // thrown away: the Listener already fed the screens with this message (a CLI reply is ours)
        stats_.drained++;
        if (rr_ && m.msg_ok && m.txt_type == MF_TXT_CLI_DATA) {
            mesh_ob_enter();
            bool ours = rr_on_message(rr_, &m, (uint32_t)millis());
            mesh_ob_exit();
            stats_.cli_replies += ours;
        }
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
    case MF_TRACE:
    case MF_DISCOVERY: {
        if (!mt_) {
            break;
        }
        uint32_t now = (uint32_t)millis();
        mesh_ob_enter();
        bool ours = m.kind == MF_TRACE ? mt_on_trace(mt_, &m.trace, now) : mt_on_discovery(mt_, &m.disc, now);
        mesh_ob_exit();
        if (ours) {
            (m.kind == MF_TRACE ? stats_.traces : stats_.discoveries)++;
        }
        if (ours && m.kind == MF_DISCOVERY) {
            store_discovered_path(m.disc);
        }
        break;
    }
    case MF_LOGIN_SUCCESS:
    case MF_LOGIN_FAIL:
    case MF_TELEMETRY:
    case MF_STATUS:
        if (rr_) {
            mesh_ob_enter();
            bool ours = m.kind == MF_STATUS      ? rr_on_status(rr_, &m, (uint32_t)millis())
                        : m.kind == MF_TELEMETRY ? rr_on_telemetry(rr_, &m, (uint32_t)millis())
                                                 : rr_on_login(rr_, &m, (uint32_t)millis());
            mesh_ob_exit();
            stats_.remote_answers += ours;
        }
        break;
    default:
        break; // adverts, path updates: counted, #47 and later issues use them
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

void BadgeMesh::fillHealth(mesh_health_t *h) {
    h->have_last     = radio_driver.getPacketsRecv() > 0;
    h->last_rssi_x10 = (int16_t)lroundf(radio_driver.getLastRSSI() * 10.0f);
    h->last_snr_x10  = (int16_t)lroundf(radio_driver.getLastSNR() * 10.0f);
    h->tx_air_ms     = (uint32_t)getTotalAirTime();
    h->rx_air_ms     = (uint32_t)getReceiveAirTime();
    h->uptime_s      = (uint32_t)(millis() / 1000);
    h->recv_flood    = getNumRecvFlood();
    h->recv_direct   = getNumRecvDirect();
    h->sent_flood    = getNumSentFlood();
    h->sent_direct   = getNumSentDirect();
    h->recv_errors   = radio_driver.getPacketsRecvErrors();
    h->err_flags     = _err_flags;
    int queued       = _mgr->getOutboundTotal();
    h->queue_len     = (uint8_t)(queued < 0 ? 0 : queued > 255 ? 255 : queued);
}
