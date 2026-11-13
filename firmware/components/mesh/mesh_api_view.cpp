// MeshCore's messages, chats and contacts for the screens: copies taken under the mesh lock.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"

using namespace mesh_int;

extern "C" size_t mesh_messages_snapshot(mesh_msg_t *out, size_t max, uint32_t *generation, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n    = mesh_store_recent(out, max);
    *generation = mesh_store_view_generation();
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_chat_snapshot(mesh_chat_key_t const *key, mesh_msg_t *out, size_t max, uint32_t *generation,
                                     bool mark_read, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n    = mesh_store_chat(key, out, max, mark_read);
    *generation = mesh_store_view_generation();
    if (mark_read) {
        tile_publish_unread();
    }
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_convs_snapshot(mesh_conv_t *out, size_t max, uint32_t wait_ms) {
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t n = mesh_store_convs(out, max);
    for (size_t i = 0; i < n; i++) { // a removed contact's DM history stays, marked (#101)
        out[i].contact_gone = out[i].key.kind == MESH_CHAT_DM && node && !node->lookupContactByPubKey(out[i].key.id, 6);
    }
    mesh_unlock();
    return n;
}

// Keeps out[0..*n) in the view's order, dropping the last once max are in
static void contact_insert(mesh_contact_view_t view, mesh_contact_t const *entry, mesh_contact_t *out, size_t *n,
                           size_t max) {
    size_t pos = *n;
    while (pos > 0 && mesh_contact_view_before(view, entry->flags, entry->age_s, out[pos - 1].flags, out[pos - 1].age_s)) {
        pos--;
    }
    if (pos >= max) {
        return;
    }
    size_t move = (*n < max ? *n : max - 1) - pos;
    memmove(&out[pos + 1], &out[pos], move * sizeof(*out));
    out[pos] = *entry;
    *n       = *n < max ? *n + 1 : max;
}

extern "C" size_t mesh_contacts_view(mesh_contact_view_t view, mesh_contact_t *out, size_t max, size_t *total,
                                     uint32_t wait_ms) {
    size_t n = 0;
    *total   = 0;
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    uint32_t         now = rtc_clock.getCurrentTime();
    ContactsIterator it  = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        if (!mesh_contact_view_keeps(view, c.type)) {
            continue;
        }
        (*total)++;
        if (max == 0) {
            continue; // only counting
        }
        mesh_contact_t entry;
        memcpy(entry.pubkey, c.id.pub_key, sizeof(entry.pubkey));
        mesh_sanitize(c.name, strnlen(c.name, sizeof(c.name)), entry.name, sizeof(entry.name));
        entry.type     = c.type;
        entry.flags    = c.flags;
        entry.path_len = c.out_path_len;
        entry.age_s    = now > c.lastmod ? now - c.lastmod : 0;
        contact_insert(view, &entry, out, &n, max);
    }
    mesh_unlock();
    return n;
}

extern "C" bool mesh_contacts_stats(mesh_contact_stats_t *out, uint32_t wait_ms) {
    mesh_contact_stats_t st = {};
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE)) {
        *out = st;
        return true;
    }
    if (!mesh_lock_within(wait_ms)) {
        return false;
    }
    uint32_t         now = rtc_clock.getCurrentTime();
    ContactsIterator it  = node->startContactsIterator();
    ContactInfo      c;
    while (it.hasNext(node, c)) {
        uint32_t age = now > c.lastmod ? now - c.lastmod : 0;
        st.total++;
        st.repeaters += mesh_contact_view_keeps(MESH_VIEW_REPEATERS, c.type) ? 1 : 0;
        st.newest_age_s = !st.heard_any || age < st.newest_age_s ? age : st.newest_age_s;
        st.heard_any    = true;
    }
    mesh_unlock();
    *out = st;
    return true;
}

extern "C" size_t mesh_contacts_snapshot(mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms) {
    if (max == 0) {
        *total = 0;
        return 0; // as before: nothing asked, nothing counted
    }
    return mesh_contacts_view(MESH_VIEW_ALL, out, max, total, wait_ms);
}

// ---- one contact: the details screen (#49) ---------------------------------------------------------------------------

extern "C" size_t mesh_contact_detail(uint8_t const prefix[6], mesh_contact_detail_t *out, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY; // not up (yet), or stopped for an update: not a sign the contact is gone
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    ContactInfo const *c = node->lookupContactByPubKey(prefix, MF_PREFIX);
    if (c) {
        uint32_t now = rtc_clock.getCurrentTime();
        memcpy(out->pub, c->id.pub_key, sizeof(out->pub));
        mesh_sanitize(c->name, strnlen(c->name, sizeof(c->name)), out->name, sizeof(out->name));
        out->type     = c->type;
        out->flags    = c->flags;
        out->path_len = c->out_path_len;
        memcpy(out->path, c->out_path, sizeof(out->path));
        out->age_s  = now > c->lastmod ? now - c->lastmod : 0;
        out->lat_e6 = c->gps_lat;
        out->lon_e6 = c->gps_lon;
    }
    mesh_unlock();
    return c ? 1 : 0;
}

// The contacts' advertised positions for the Contact radar (#171): one pass under the lock, no copy of what it does not need
extern "C" size_t mesh_contacts_positions(radar_contact_t *out, size_t max, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped() || !mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t           n  = 0;
    ContactsIterator it = node->startContactsIterator();
    ContactInfo      c;
    while (n < max && it.hasNext(node, c)) {
        if (c.gps_lat == 0 && c.gps_lon == 0) {
            continue;
        }
        radar_contact_t *e = &out[n++];
        memcpy(e->prefix, c.id.pub_key, sizeof(e->prefix));
        mesh_sanitize(c.name, strnlen(c.name, sizeof(c.name)), e->name, sizeof(e->name));
        e->type   = c.type;
        e->lat_e6 = c.gps_lat;
        e->lon_e6 = c.gps_lon;
    }
    mesh_unlock();
    return n;
}

extern "C" size_t mesh_contact_do(uint8_t const prefix[6], mesh_contact_op_t op, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY; // as mesh_contact_detail
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    BadgeMesh::ContactResult r = node->contactAction(prefix, op);
    mesh_unlock();
    return r == BadgeMesh::CONTACT_QUEUED ? 1 : r == BadgeMesh::CONTACT_GONE ? 0 : MESH_BUSY;
}

extern "C" size_t mesh_contact_set_path(uint8_t const prefix[6], uint8_t path_len,
                                        uint8_t const path[MESH_CONTACT_PATH_MAX], uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    BadgeMesh::ContactResult r = node->contactSetPath(prefix, path_len, path);
    mesh_unlock();
    return r == BadgeMesh::CONTACT_QUEUED ? 1 : r == BadgeMesh::CONTACT_GONE ? 0 : MESH_BUSY;
}
