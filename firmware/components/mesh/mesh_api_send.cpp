// Sending (#45), the outbox list, trace and discovery (#53), and remote requests (#51, #162).
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"

using namespace mesh_int;

static char const TAG[] = "mesh";

// ---- sending (#45) -------------------------------------------------------------------------------------------------

extern "C" size_t mesh_channel_text_max(void) {
    mesh_settings_t st;
    mesh_get_settings(&st);
    return mf_channel_text_max(strlen(st.name));
}

// from: a console line's send is refused while the console scope is not set (#290), never sent unscoped
bool mesh_int::send_ready(char const **why, mesh_origin_t from) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || !outbox || mesh_is_stopped()) {
        *why = stopped_why();
        return false;
    }
    if ((*why = mesh_cscope_refusal(from == MESH_FROM_CONSOLE)) != NULL) {
        return false;
    }
    if (!ident_tx_allowed()) {
        *why = "identity damaged: TX blocked"; // refused here, not queued to fail at the radio
        return false;
    }
    if (!lichen_radio_get(RADIO_LORA)) { // the one gate for "radio off" (#108); startSendRaw refuses too, as a net
        *why = lichen_airplane_get() ? "LoRa is off (airplane mode)" : "LoRa is off";
        return false;
    }
    return true;
}

// #289: a console line's send goes only to a target on the console's on-air allow list (mesh_onair.h), checked after
// send_ready (its reasons come first) and before anything is queued; NULL from a screen and while the list is off.
// The frame is checked again when it is built (onair_frame_ok in mesh_link.cpp), retries included.
static bool onair_ready(char const **why, mesh_origin_t from, uint8_t const *prefix) {
    *why = mesh_onair_refusal(from == MESH_FROM_CONSOLE, ONAIR_TX_CONTACT, NULL, NULL, prefix);
    return *why == NULL;
}

// The same for a channel: the key of the slot the stored name resolves to now, under the mesh lock, only while the
// list is on (off: no lock taken, as before)
static bool onair_channel_ready(char const **why, mesh_origin_t from, char const *channel) {
    if (from != MESH_FROM_CONSOLE || !mesh_onair_on()) {
        return true;
    }
    char    name[MESH_CHANNEL_MAX + 1];
    uint8_t id[ONAIR_KEY_ID];
    size_t  found = mesh_channel_key_id(channel, false, name, sizeof(name), id);
    if (found == MESH_BUSY) {
        *why = "MeshCore is busy, try again";
        return false;
    }
    *why = mesh_onair_refusal(true, ONAIR_TX_CHANNEL, channel, found == 1 ? id : NULL, NULL);
    return *why == NULL;
}

extern "C" mesh_msg_id_t mesh_send_channel(char const *channel_name, char const *text, mesh_origin_t from,
                                           char const **why) {
    char const *local;
    why = why ? why : &local;
    *why = NULL;
    if (!send_ready(why, from) || (*why = mesh_cscope_channel_refusal(from == MESH_FROM_CONSOLE, channel_name)) ||
        !onair_channel_ready(why, from, channel_name)) {
        return 0; // #291: a damaged channel scope refuses the screens' sends
    }
    size_t len = mf_text_cut(text, mesh_channel_text_max());
    if (len == 0 && text[0]) {
        *why = "no room for text";
        return 0;
    }
    mesh_ob_enter();
    mesh_msg_id_t id = ob_add_channel(outbox, channel_name, text, len, (uint32_t)millis(), why);
    if (id && from == MESH_FROM_CONSOLE) {
        ob_set_console(outbox, id); // in the same section as the add: the pipe never takes it unmarked
    }
    mesh_ob_exit();
    return id;
}

extern "C" mesh_msg_id_t mesh_send_dm(uint8_t const prefix[6], char const *text, mesh_origin_t from, char const **why) {
    char const *local;
    why = why ? why : &local;
    *why = NULL;
    if (!send_ready(why, from) || !onair_ready(why, from, prefix)) {
        return 0;
    }
    size_t len = mf_text_cut(text, MF_MAX_TEXT);
    mesh_ob_enter();
    mesh_msg_id_t id = ob_add_dm(outbox, prefix, text, len, (uint32_t)millis(), why);
    if (id && from == MESH_FROM_CONSOLE) {
        ob_set_console(outbox, id);
    }
    mesh_ob_exit();
    return id;
}

extern "C" bool mesh_retry(mesh_msg_id_t id) {
    char const *why;
    if (!send_ready(&why)) {
        return false;
    }
    mesh_ob_enter();
    bool ok = ob_retry(outbox, id, (uint32_t)millis());
    mesh_ob_exit();
    return ok;
}

extern "C" bool mesh_out_state(mesh_msg_id_t id, mesh_out_state_t *st, uint8_t *attempt, uint8_t *repeats) {
    if (!outbox) {
        return false;
    }
    mesh_ob_enter();
    bool ok = ob_get(outbox, id, st, attempt, repeats, NULL);
    mesh_ob_exit();
    return ok;
}

// Copies what the list needs under the outbox lock; the formatting happens after it, so the lock is held only briefly
struct OutSnap {
    mesh_out_info_t info;
    uint8_t         prefix[MF_PREFIX];
    char            target[OB_TARGET_MAX];
    uint32_t        seq;
};

// The copy is in PSRAM (#208): OB_CAP of them made a 1.7 KB frame on the caller's stack (the console's is 4 KB).
// Taken before the outbox lock, which never waits on anything.
extern "C" size_t mesh_out_list(mesh_out_info_t *out, size_t max) {
    size_t n = 0;
    if (!outbox) {
        return 0;
    }
    OutSnap *snap = (OutSnap *)lichen_alloc(meshcore_ctx(), OB_CAP * sizeof(OutSnap), LICHEN_MEM_BULK);
    if (!snap) {
        ESP_LOGW(TAG, "outbox list: no memory for the copy");
        return MESH_OUT_NO_MEMORY;
    }
    mesh_ob_enter();
    for (int i = 0; i < OB_CAP; i++) {
        ob_entry_t const *e = &outbox->e[i];
        if (!e->used) {
            continue;
        }
        OutSnap &s       = snap[n++];
        s.info.id        = e->id;
        s.info.is_dm     = e->kind == OB_DM;
        s.info.state     = (mesh_out_state_t)e->state;
        s.info.attempt   = e->attempt;
        s.info.repeats   = e->repeats;
        s.info.trip_ms   = e->trip_ms;
        s.seq            = e->seq;
        memcpy(s.prefix, e->prefix, MF_PREFIX);
        memcpy(s.target, e->target, sizeof(s.target));
    }
    mesh_ob_exit();
    for (size_t i = 1; i < n; i++) { // newest first
        OutSnap t = snap[i];
        size_t  k = i;
        for (; k > 0 && (int32_t)(t.seq - snap[k - 1].seq) > 0; k--) {
            snap[k] = snap[k - 1];
        }
        snap[k] = t;
    }
    if (n > max) {
        n = max;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = snap[i].info;
        if (snap[i].info.is_dm) {
            snprintf(out[i].target, sizeof(out[i].target), "%02x%02x%02x%02x%02x%02x", snap[i].prefix[0],
                     snap[i].prefix[1], snap[i].prefix[2], snap[i].prefix[3], snap[i].prefix[4], snap[i].prefix[5]);
        } else {
            snprintf(out[i].target, sizeof(out[i].target), "%s", snap[i].target);
        }
    }
    lichen_free(meshcore_ctx(), snap);
    return n;
}

// ---- trace path and path discovery (#53) ---------------------------------------------------------------------------
// One request at a time, shared with the pipe under the outbox lock: copy-in, never the mesh lock (LVGL safe)

mt_t *mesh_int::trace_req; // PSRAM, made with the outbox

extern "C" uint32_t mesh_trace_start(uint8_t const prefix[6], mt_kind_t kind, mesh_origin_t from, char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why, from) || !onair_ready(why, from, prefix)) {
        return 0;
    }
    if (!trace_req) {
        *why = "MeshCore is not running";
        return 0;
    }
    uint32_t tag;
    lichen_random_fill(&tag, sizeof(tag)); // a trace is matched by its tag: random, as MeshCore's apps make it
    mesh_ob_enter();
    uint32_t seq = mt_request(trace_req, kind, prefix, tag, (uint32_t)millis(), why);
    trace_req->console = seq ? from == MESH_FROM_CONSOLE : trace_req->console; // with the request it belongs to
    mesh_ob_exit();
    return seq;
}

extern "C" bool mesh_trace_get(mt_t *out) {
    if (!trace_req) {
        return false;
    }
    mesh_ob_enter();
    mt_poll(trace_req, (uint32_t)millis(), out); // a plain copy: the lines are made after the lock
    mesh_ob_exit();
    return true;
}

// ---- log in, status and CLI for a repeater or room (#51) -------------------------------------------------------------
// One request at a time, shared with the pipe under the outbox lock as the trace: copy-in, never the mesh lock

rr_t *mesh_int::remote_req; // PSRAM, made with the outbox
tl_rate_t mesh_int::telem_rate; // when each contact was last asked for telemetry (#162), under mesh_ob_enter/exit

extern "C" uint32_t mesh_remote_start(uint8_t const prefix[6], rr_op_t op, char const *arg, mesh_origin_t from,
                                      char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why, from) || !onair_ready(why, from, prefix)) {
        return 0;
    }
    if (!remote_req) {
        *why = "MeshCore is not running";
        return 0;
    }
    rr_prep_t prep; // classified and redacted here, outside the critical section (#117)
    rr_prepare(op, arg, &prep);
    mesh_ob_enter();
    uint32_t seq = rr_request_prepared(remote_req, op, prefix, arg, &prep, (uint32_t)millis(), why);
    remote_req->console = seq ? from == MESH_FROM_CONSOLE : remote_req->console;
    mesh_ob_exit();
    return seq;
}

// The status history's poll (#168): as mesh_remote_start for a status request, but marked as background, so a request
// of the person replaces it (rr_request_background)
extern "C" uint32_t mesh_remote_start_background(uint8_t const prefix[6], mesh_origin_t from, char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why, from) || !onair_ready(why, from, prefix)) {
        return 0;
    }
    if (!remote_req) {
        *why = "MeshCore is not running";
        return 0;
    }
    mesh_ob_enter();
    uint32_t seq = rr_request_background(remote_req, prefix, (uint32_t)millis(), why);
    remote_req->console = seq ? from == MESH_FROM_CONSOLE : remote_req->console;
    mesh_ob_exit();
    return seq;
}

extern "C" uint32_t mesh_telemetry_start(uint8_t const prefix[6], mesh_origin_t from, char *why, size_t cap) {
    why[0] = 0;
    uint32_t wait = mesh_telemetry_wait_s(prefix);
    if (wait) {
        snprintf(why, cap, "Wait %lu s", (unsigned long)wait);
        return 0;
    }
    char const *reason = NULL;
    uint32_t    seq    = mesh_remote_start(prefix, RR_TELEMETRY, NULL, from, &reason);
    if (seq) {
        mesh_ob_enter();
        tl_rate_note(&telem_rate, prefix, (uint32_t)millis());
        mesh_ob_exit();
    } else {
        snprintf(why, cap, "%s", reason ? reason : "?");
    }
    return seq;
}

extern "C" uint32_t mesh_telemetry_wait_s(uint8_t const prefix[6]) {
    mesh_ob_enter();
    uint32_t wait = tl_rate_wait_s(&telem_rate, prefix, (uint32_t)millis());
    mesh_ob_exit();
    return wait;
}

extern "C" bool mesh_remote_head(rr_head_t *out) {
    if (!remote_req) {
        return false;
    }
    rr_head_t h; // the lock is held for this small copy only
    mesh_ob_enter();
    rr_poll_head(remote_req, (uint32_t)millis(), &h);
    mesh_ob_exit();
    *out = h;
    return true;
}

extern "C" bool mesh_remote_get(rr_t *out) {
    if (!remote_req) {
        return false;
    }
    mesh_ob_enter();
    rr_poll(remote_req, (uint32_t)millis(), out); // a plain copy (without the argument): the text is made outside
    mesh_ob_exit();
    return true;
}

extern "C" bool mesh_remote_logout(uint8_t const prefix[6], uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped() || !remote_req || !mesh_lock_within(wait_ms)) {
        return false;
    }
    bool ok = node->remoteLogout(prefix);
    mesh_unlock();
    if (ok) {
        mesh_ob_enter();
        rr_logout(remote_req, prefix);
        mesh_ob_exit();
    }
    return ok;
}
