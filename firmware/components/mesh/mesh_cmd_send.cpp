// "fw mesh send|dm|path|trace|discover|out|link|chats".
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"
#include "mesh_cmd.h"

using namespace mesh_int;

static char const *const OUT_STATE_NAME[] = {"queued", "sent", "on air", "heard repeated", "delivered", "FAILED"};

// A channel name typed on the console as stored (channel_find_locked), into out (may be typed); false (said why,
// after `what`) when there is none or MeshCore is not there
bool mesh_int::console_channel_name(char const *typed, char *out, size_t cap, char const *what) {
    if (char const *why = settings_lock()) {
        console_reply("%s: %s", what, why);
        return false;
    }
    ChannelDetails found;
    int            slot = channel_find_locked(typed);
    bool           have = slot >= 0 && node->getChannel(slot, found);
    mesh_unlock();
    if (!have) {
        console_reply("%s: no channel %s (\"fw channels\" lists them)", what, typed);
        return false;
    }
    snprintf(out, cap, "%.*s", (int)sizeof(found.name), found.name);
    return true;
}

// "fw mesh send <channel> <text>"
void mesh_int::cmd_mesh_send(char const *args) {
    char channel[OB_TARGET_MAX] = "";
    while (*args == ' ') {
        args++;
    }
    size_t name_len = strcspn(args, " ");
    int    used     = (int)name_len;
    while (args[used] == ' ') {
        used++;
    }
    if (name_len == 0 || !args[used]) {
        console_reply("mesh send <channel name> <text>");
        return;
    }
    if (name_len >= OB_TARGET_MAX) {
        console_reply("channel name longer than %d bytes", OB_TARGET_MAX - 1);
        return;
    }
    memcpy(channel, args, name_len);
    if (!console_channel_name(channel, channel, sizeof(channel), "not queued")) {
        return; // the outbox names the channel as stored: "badgetest" means "#badgetest" (#101)
    }
    if (!clock_is_set()) {
        console_reply("warning: the clock is not set, other nodes will see a wrong time");
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = mesh_send_channel(channel, args + used, MESH_FROM_CONSOLE, &why);
    if (!id) {
        console_reply("not queued: %s", why ? why : "?");
    } else {
        size_t max = mesh_channel_text_max();
        console_reply("queued #%lu on %s (max %u bytes%s)", (unsigned long)id, channel, (unsigned)max,
                      strlen(args + used) > max ? ", text cut" : "");
    }
}

// "fw mesh dm <12 hex digits of the public key> <text>"
void mesh_int::cmd_mesh_dm(char const *args) {
    uint8_t prefix[6];
    int     used = 12;
    unsigned b[6];
    bool ok = strspn(args, "0123456789abcdefABCDEF") == 12 && args[12] == ' ';
    while (ok && args[used] == ' ') {
        used++;
    }
    if (!ok || !args[used] ||
        sscanf(args, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) < 6) {
        console_reply("mesh dm <12 hex digits of the public key> <text>");
        return;
    }
    for (int i = 0; i < 6; i++) {
        prefix[i] = (uint8_t)b[i];
    }
    char const   *why = NULL;
    mesh_msg_id_t id  = mesh_send_dm(prefix, args + used, MESH_FROM_CONSOLE, &why);
    if (id) {
        console_reply("queued #%lu", (unsigned long)id);
    } else {
        console_reply("not queued: %s", why ? why : "?");
    }
}

// A contact by the start of its key as typed (2 to 12 hex digits): false (said why) when none or more than one
bool mesh_int::console_contact(char const *hex, uint8_t out[6]) {
    size_t   len = strspn(hex, "0123456789abcdefABCDEF");
    uint8_t  want[6];
    unsigned b;
    if (len < 2 || len > 12 || len % 2 || hex[len]) {
        console_reply("give 2 to 12 hex digits of the contact's key (fw contacts lists them)");
        return false;
    }
    for (size_t i = 0; i < len / 2; i++) {
        sscanf(hex + 2 * i, "%2x", &b);
        want[i] = (uint8_t)b;
    }
    if (!mesh_lock_within(MESH_SET_WAIT_MS)) {
        console_reply("MeshCore is busy, try again");
        return false;
    }
    int              found = 0;
    ContactsIterator it    = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        if (memcmp(c.id.pub_key, want, len / 2) == 0 && found++ == 0) {
            memcpy(out, c.id.pub_key, 6);
        }
    }
    mesh_unlock();
    if (found > 1) {
        console_reply("%d contacts start with %s: give more digits", found, hex);
    } else if (!found) {
        console_reply("no contact starts with %s", hex);
    }
    return found == 1;
}

// "fw mesh path <hex>" shows the stored path, "... reset" forgets it (the screen's Reset path), "... set <hop>[,<hop>..]"
// stores one by hand (#130). Nothing is sent: the next DM goes along it, or floods when there is none.
void mesh_int::cmd_mesh_path(char const *args) {
    while (*args == ' ') {
        args++;
    }
    char hex[16];
    size_t hl = strcspn(args, " ");
    if (hl == 0 || hl >= sizeof(hex)) {
        console_reply("mesh path <12 hex digits of the key> [reset | set <hop>[,<hop>...]]");
        return;
    }
    memcpy(hex, args, hl);
    hex[hl] = 0;
    char const *rest = args + hl;
    while (*rest == ' ') {
        rest++;
    }
    uint8_t prefix[6];
    if (!console_contact(hex, prefix)) {
        return;
    }
    size_t r = 1;
    if (strcmp(rest, "reset") == 0) {
        r = mesh_contact_do(prefix, MESH_CO_RESET_PATH, MESH_SET_WAIT_MS);
    } else if (strncmp(rest, "set", 3) == 0 && (rest[3] == ' ' || !rest[3])) {
        char const *hops = rest + 3;
        while (*hops == ' ') {
            hops++;
        }
        mesh_settings_t st;
        mesh_get_settings(&st);
        uint8_t         len, path[MESH_CONTACT_PATH_MAX];
        mesh_path_err_t e = mesh_path_parse(hops, st.path_hash_size, &len, path);
        if (e != MESH_PATH_OK) {
            console_reply("path not set (%u B hops): %s", st.path_hash_size, mesh_path_err_text(e));
            return;
        }
        r = mesh_contact_set_path(prefix, len, path, MESH_SET_WAIT_MS);
    } else if (*rest) {
        console_reply("mesh path <hex> [reset | set <hop>[,<hop>...]]");
        return;
    }
    if (r != 1) {
        console_reply(r == 0 ? "that contact is gone" : "MeshCore is busy, try again");
        return;
    }
    mesh_contact_detail_t d;
    if (mesh_contact_detail(prefix, &d, MESH_SET_WAIT_MS) != 1) {
        console_reply("path of that contact is not readable now");
        return;
    }
    mesh_path_t p = mesh_path_decode(d.path_len);
    if (p.flood) {
        console_reply("path %s: flood", d.name);
        return;
    }
    // mesh_hex groups by hash with spaces: the hops, comma separated
    char list[MESH_CONTACT_PATH_MAX * 2 + MESH_CONTACT_PATH_MAX + 1];
    mesh_hex(d.path, (size_t)p.hops * p.hash_size, p.hash_size, list, sizeof(list));
    for (char *q = list; *q; q++) {
        if (*q == ' ') {
            *q = ',';
        }
    }
    console_reply("path %s: direct, %u hop%s, %u B%s%s", d.name, p.hops, p.hops == 1 ? "" : "s", p.hash_size,
                  p.hops ? ": " : "", list);
}

static char const *const TRACE_STATE_NAME[] = {"none", "queued", "sent to MeshCore", "waiting", "done", "failed",
                                               "no answer"};

// "fw mesh trace|discover <hex>": starts one; "fw mesh trace" alone shows the last one and its result
void mesh_int::cmd_mesh_trace(char const *arg, mt_kind_t kind) {
    if (*arg) {
        uint8_t prefix[6];
        if (!console_contact(arg, prefix)) {
            return;
        }
        char const *why = NULL;
        uint32_t    seq = mesh_trace_start(prefix, kind, MESH_FROM_CONSOLE, &why);
        if (seq) {
            console_reply("%s #%lu queued: fw mesh trace shows the result", kind == MT_TRACE ? "trace" : "discover",
                          (unsigned long)seq);
        } else {
            console_reply("not started: %s", why ? why : "?");
        }
        return;
    }
    mt_t      *t     = (mt_t *)lichen_alloc(meshcore_ctx(), sizeof(mt_t), LICHEN_MEM_BULK);
    mt_line_t *lines = (mt_line_t *)lichen_alloc(meshcore_ctx(), MT_LINES_MAX * sizeof(mt_line_t), LICHEN_MEM_BULK);
    if (t && lines && mesh_trace_get(t)) {
        console_reply("%s #%lu to %02x%02x%02x%02x%02x%02x: %s", t->kind == MT_TRACE ? "trace" : "discover",
                      (unsigned long)t->seq, t->prefix[0], t->prefix[1], t->prefix[2], t->prefix[3], t->prefix[4],
                      t->prefix[5], TRACE_STATE_NAME[t->state]);
        size_t n = t->seq ? mt_format(t, lines, MT_LINES_MAX) : 0;
        for (size_t i = 0; i < n; i++) {
            console_reply("  %s", lines[i].text);
        }
    }
    lichen_free(meshcore_ctx(), t);
    lichen_free(meshcore_ctx(), lines);
}

void mesh_int::cmd_mesh_out(void) {
    mesh_out_info_t list[OB_CAP];
    size_t          n = mesh_out_list(list, OB_CAP);
    for (size_t i = 0; i < n; i++) {
        console_reply("out #%lu %s %s: %s, attempt %u, repeats %u, trip %lu ms", (unsigned long)list[i].id,
                      list[i].is_dm ? "dm" : "channel", list[i].target, OUT_STATE_NAME[list[i].state], list[i].attempt,
                      list[i].repeats, (unsigned long)list[i].trip_ms);
    }
    console_reply("%u outgoing message(s)", (unsigned)n);
}

// "fw mesh link": the companion pipe's counters (#45, #55)
void mesh_int::cmd_mesh_link(void) {
    mesh_lock();
    LinkStats s    = node->pipe().stats();
    bool      busy = node->pipe().draining();
    mesh_unlock();
    console_reply("link connected, protocol %u | cmds out %lu, replies %lu, pushes %lu | self ok %lu bad %lu", s.app_ver,
                  (unsigned long)s.cmds_out, (unsigned long)s.replies, (unsigned long)s.pushes,
                  (unsigned long)s.self_ok, (unsigned long)s.self_bad);
    console_reply("offline queue %d (drained %lu, msg-waiting pushes %lu, draining %d) | lost replies %lu, fifo drops %lu, "
                  "bad frames %lu, send confirmed %lu",
                  (int)offline_queue_len, (unsigned long)s.drained, (unsigned long)s.msg_waiting, busy,
                  (unsigned long)s.lost_replies, (unsigned long)s.fifo_drops, (unsigned long)s.bad_frames,
                  (unsigned long)s.confirmed);
    console_reply("traces back %lu, discoveries answered %lu, paths stored %lu", (unsigned long)s.traces,
                  (unsigned long)s.discoveries, (unsigned long)s.paths_stored);
    console_reply("repeater/room answers %lu, CLI replies %lu", (unsigned long)s.remote_answers,
                  (unsigned long)s.cli_replies);
    for (int i = 0; i < LINK_PUSH_KINDS; i++) {
        if (s.push_count[i]) {
            console_reply("  push 0x%02x x %lu", 0x80 + i, (unsigned long)s.push_count[i]);
        }
    }
    console_reply("mesh task stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(mesh_task_handle));
}

// "fw mesh chats": every chat with its unread count and how full its ring is
void mesh_int::cmd_mesh_chats(void) {
    mesh_conv_t *list = (mesh_conv_t *)lichen_alloc(meshcore_ctx(), MESH_CONV_MAX * sizeof(mesh_conv_t), LICHEN_MEM_BULK);
    if (!list) {
        console_reply("no memory");
        return;
    }
    size_t n = mesh_convs_snapshot(list, MESH_CONV_MAX, MESH_WAIT_FOREVER);
    for (size_t i = 0; i < n; i++) {
        char what[16];
        if (list[i].key.kind == MESH_CHAT_DM) {
            snprintf(what, sizeof(what), "dm %02x%02x%02x%02x%02x%02x", list[i].key.id[0], list[i].key.id[1],
                     list[i].key.id[2], list[i].key.id[3], list[i].key.id[4], list[i].key.id[5]);
        } else {
            snprintf(what, sizeof(what), "channel");
        }
        console_reply("chat %-15s %-32s unread %u, %u/%u kept", what, list[i].title, (unsigned)list[i].unread,
                      (unsigned)list[i].count, (unsigned)STORE_MESSAGES);
    }
    lichen_free(meshcore_ctx(), list);
    console_reply("%u chat(s), %u unread", (unsigned)n, (unsigned)tile_unread());
}
