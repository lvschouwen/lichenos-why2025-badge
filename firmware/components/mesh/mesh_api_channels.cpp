// MeshCore's channels (#54) and the contact export / import links.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"

using namespace mesh_int;

static char const TAG[] = "mesh";

// MeshCore's hashtag channels: the key is the first 16 bytes of SHA-256 of the name, '#' included, lowercase
static size_t hashtag_secret(char const *name, uint8_t out[CHANNEL_SECRET_MAX]) {
    uint8_t digest[32];
    SHA256  sha;
    sha.update(name, strlen(name));
    sha.finalize(digest, sizeof(digest));
    memcpy(out, digest, 16);
    return 16;
}

extern "C" char const *mesh_channel_add(char const *name_in, char const *b64_key_or_null) {
    ChannelDetails details = {};
    char           name[MESH_CHANNEL_MAX + 1];
    size_t         key_len;
    // The input rules (and the key's bounds, #65 review) are channel_input.c's, host-tested
    char const *why = channel_parse(name_in, b64_key_or_null, name, sizeof(name), details.channel.secret, &key_len);
    if (why) {
        return why;
    }
    if (key_len == 0) {
        key_len = hashtag_secret(name, details.channel.secret);
    }
    StrHelper::strncpy(details.name, name, sizeof(details.name));
    mesh::Utils::sha256(details.channel.hash, sizeof(details.channel.hash), details.channel.secret, (int)key_len);

    why = settings_lock();
    if (why) {
        return why;
    }
    // As the app's CMD_SET_CHANNEL: the first free slot, then saved (addChannel() counts only its own additions and
    // would overwrite channels loaded from flash)
    ChannelDetails slot;
    int            free_slot = -1;
    bool           exists    = false;
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
        if (!slot.name[0]) {
            free_slot = free_slot < 0 ? i : free_slot;
        } else if (channel_same_name(slot.name, details.name)) {
            exists = true;
        }
    }
    bool ok = !exists && free_slot >= 0 && node->setChannel(free_slot, details);
    if (ok) {
        store.saveChannels(node);
        node->noteChannelScopes(); // #291: the senders' copy of the damaged channels
    }
    mesh_unlock();
    if (exists) {
        return "That channel is already there";
    }
    return ok ? NULL : "No free channel slot";
}

// The slot of the channel a typed name means (channel_name_match: "badgetest" finds "#badgetest", an exact name wins),
// -1 for none. With the lock held.
int mesh_int::channel_find_locked(char const *typed) {
    ChannelDetails slot;
    int            best = -1, best_match = CHANNEL_MATCH_NONE;
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
        int match = slot.name[0] ? channel_name_match(slot.name, typed) : CHANNEL_MATCH_NONE;
        if (match > best_match) {
            best = i, best_match = match;
        }
    }
    return best;
}

// The slot of the channel stored as `name` exactly, without case (the screens pass the stored name), -1 for none.
// With the lock held; slot gets that channel.
static int channel_named_locked(char const *name, ChannelDetails &slot) {
    for (int i = 0; i < MAX_GROUP_CHANNELS && node->getChannel(i, slot); i++) {
        if (slot.name[0] && channel_same_name(slot.name, name)) {
            return i;
        }
    }
    return -1;
}

extern "C" size_t mesh_channel_key_id(char const *channel, bool typed, char *name, size_t cap,
                                      uint8_t key_id[ONAIR_KEY_ID]) {
    if (settings_lock()) {
        return MESH_BUSY;
    }
    ChannelDetails slot;
    int            at    = typed ? channel_find_locked(channel) : channel_named_locked(channel, slot);
    bool           found = at >= 0 && node->getChannel(at, slot) && slot.name[0];
    if (found) {
        channel_key_id(slot.channel.secret, key_id);
        snprintf(name, cap, "%.*s", (int)sizeof(slot.name), slot.name); // name may be the channel argument
    }
    mesh_unlock();
    mesh_wipe(&slot, sizeof(slot)); // the channel's key: not left on the stack
    return found ? 1 : 0;
}

// Clears slot `index` as MeshCore's apps do (setChannel with an empty ChannelDetails) and saves. DataStore writes every
// slot in order, empty ones as zeroes, and loads them back by position: so the later channels keep their indices and
// an emptied slot reads back empty (the lists skip it, the next add reuses it). Called with the lock held; by_name
// (or NULL) picks the slot by name, without case, instead: the stored name exactly (the screens pass it; a typed one
// goes through channel_find_locked first, so a vanished "x" never takes "#x" with it).
static char const *channel_remove_locked(int index, char const *by_name) {
    ChannelDetails slot;
    index = by_name ? channel_named_locked(by_name, slot) : index;
    if (index < 0 || !node->getChannel(index, slot)) {
        return "No such channel";
    }
    char const *why = channel_removable(index, slot.name);
    if (why) {
        return why;
    }
    ChannelDetails empty = {};
    if (!node->setChannel(index, empty)) {
        return "No such channel";
    }
    store.saveChannels(node);
    // #291: its own scope after the channel is gone and saved (a cut in between leaves a line the next start drops;
    // the other order would leave the channel flooding under the default)
    char const *kept = node->setChannelScope(slot.channel.secret, "");
    ESP_LOGI(TAG, "channel %d removed%s", index, kept ? " (scope line kept until restart)" : "");
    return NULL;
}

extern "C" char const *mesh_channel_remove(int index) {
    if (index == 0) {
        return channel_removable(0, "Public"); // refused before the lock: a refusal never waits on MeshCore
    }
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = channel_remove_locked(index, NULL);
    mesh_unlock();
    return why;
}

extern "C" char const *mesh_channel_remove_named(char const *name) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    why = channel_remove_locked(-1, name);
    mesh_unlock();
    return why;
}

// ---- channel details, rename (#54) ----------------------------------------------------------------------------------

extern "C" size_t mesh_channel_info(int index, char const *name, mesh_channel_info_t *out, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return MESH_BUSY;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    ChannelDetails slot;
    int            at = index >= 0 ? index : name ? channel_find_locked(name) : -1;
    bool           ok = at >= 0 && at < MAX_GROUP_CHANNELS && node->getChannel(at, slot) && slot.name[0];
    if (ok) {
        out->index = at;
        mesh_sanitize(slot.name, strnlen(slot.name, sizeof(slot.name)), out->name, sizeof(out->name));
        memcpy(out->secret, slot.channel.secret, sizeof(out->secret));
        out->key_len = (uint8_t)mesh_channel_key_len(out->secret);
        out->hash    = slot.channel.hash[0];
        snprintf(out->scope, sizeof(out->scope), "%s", node->channelScope(slot.channel.secret)); // #291
    }
    mesh_unlock();
    memset(&slot, 0, sizeof(slot)); // a channel key: not left on the stack
    return ok ? 1 : 0;
}

extern "C" char const *mesh_channel_set_scope(char const *name, char const *region) {
    char const *why = settings_lock();
    if (why) {
        return why;
    }
    ChannelDetails slot;
    int            at = channel_named_locked(name, slot);
    why               = at >= 0 ? node->setChannelScope(slot.channel.secret, region) : "No such channel";
    mesh_unlock();
    memset(&slot, 0, sizeof(slot)); // a channel key: not left on the stack
    ESP_LOGI(TAG, "channel %d flood scope: %s", at, why ? "not changed" : region[0] ? "own" : "default");
    return why;
}

extern "C" char const *mesh_channel_rename(char const *name, char const *new_in) {
    char        clean[MESH_CHANNEL_MAX + 1];
    bool        hashtag;
    char const *why = channel_rename_parse(new_in, clean, sizeof(clean), &hashtag);
    if (why) {
        return why;
    }
    why = settings_lock();
    if (why) {
        return why;
    }
    ChannelDetails slot, other;
    int            at = channel_named_locked(name, other);
    if (at < 0 || !node->getChannel(at, slot)) {
        why = "No such channel";
    }
    for (int i = 0; !why && i < MAX_GROUP_CHANNELS && node->getChannel(i, other); i++) {
        if (i != at && other.name[0] && channel_same_name(other.name, clean)) {
            why = "Another channel has that name";
        }
    }
    if (!why && hashtag) {
        uint8_t tag[CHANNEL_SECRET_MAX] = {};
        hashtag_secret(clean, tag);
        if (memcmp(tag, slot.channel.secret, sizeof(slot.channel.secret)) != 0) {
            why = "A #name needs that hashtag's key: add it as a new channel instead";
        }
        memset(tag, 0, sizeof(tag));
    }
    if (!why) {
        StrHelper::strncpy(slot.name, clean, sizeof(slot.name)); // the name only: secret and hash stay as they are
        why = node->setChannel(at, slot) ? NULL : "No such channel";
    }
    if (!why) {
        store.saveChannels(node);
        node->noteChannelScopes(); // #291: the damaged names the senders check
        ESP_LOGI(TAG, "channel %d renamed", at);
    }
    mesh_unlock();
    memset(&slot, 0, sizeof(slot));
    memset(&other, 0, sizeof(other));
    return why;
}

// ---- contact export / import (#54) ---------------------------------------------------------------------------------

#define IMPORT_WAIT_MS 3000 // MyMesh takes a loop-back packet in its next loop

extern "C" char const *mesh_contact_export(uint8_t const *prefix, char *uri, size_t cap) {
    if (!prefix && mesh_advert_refusal(clock_is_set())) {
        return "the clock is not set: our advert would carry a wrong time"; // as adverts (#8)
    }
    uint8_t *pkt = (uint8_t *)lichen_alloc(meshcore_ctx(), MAX_TRANS_UNIT, LICHEN_MEM_BULK);
    if (!pkt) {
        return "no memory";
    }
    char const *why = settings_lock();
    if (why) {
        lichen_free(meshcore_ctx(), pkt);
        return why;
    }
    int len = 0;
    if (prefix) {
        ContactInfo *c = node->lookupContactByPubKey(prefix, MF_PREFIX);
        len = c ? node->exportContact(*c, pkt) : 0; // the stored advert, a file read
        why = !c ? "No such contact" : len <= 0 ? "MeshCore kept no advert of this contact" : NULL;
    } else {
        mesh::Packet *p = node->createSelfAdvert(node->getNodeName()); // signed, not queued: never sent
        if (p) {
            p->header |= ROUTE_TYPE_FLOOD;
            len = p->writeTo(pkt);
            node->releasePacket(p);
        }
        why = p ? NULL : "MeshCore is busy, try again";
    }
    mesh_unlock();
    if (!why && !mesh_advert_uri(pkt, (size_t)len, uri, cap)) {
        why = "not an advert MeshCore can read";
    }
    lichen_free(meshcore_ctx(), pkt);
    return why;
}

extern "C" char const *mesh_contact_import(uint8_t const *pkt, size_t len, char *name_out, size_t name_cap) {
    if (len < MESH_ADVERT_PUB_AT + MESH_ADVERT_PUB_LEN || len > MAX_TRANS_UNIT) {
        return "not an advert";
    }
    uint8_t const *pub = pkt + MESH_ADVERT_PUB_AT;
    char const    *why = settings_lock();
    if (why) {
        return why;
    }
    if (memcmp(pub, node->self_id.pub_key, PUB_KEY_SIZE) == 0) {
        why = "that is this badge's own contact";
    } else if (node->lookupContactByPubKey(pub, PUB_KEY_SIZE)) {
        why = "already a contact (left as it is)";
    } else if (node->getNumContacts() >= MAX_CONTACTS) {
        why = "the contact list is full (MeshCore would replace the oldest contact)";
    } else if (!node->importContact(pkt, (uint8_t)len)) {
        why = "MeshCore did not take it (busy, or not an advert)";
    }
    mesh_unlock();
    if (why) {
        return why;
    }
    // MyMesh checks the signature and adds it in its next loop (its auto-add rules, as for an advert heard on air)
    for (uint32_t waited = 0; waited < IMPORT_WAIT_MS; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!mesh_lock_within(100)) {
            continue;
        }
        ContactInfo const *c = node->lookupContactByPubKey(pub, PUB_KEY_SIZE);
        if (c) {
            mesh_sanitize(c->name, strnlen(c->name, sizeof(c->name)), name_out, name_cap);
        }
        bool gone = mesh_is_stopped();
        mesh_unlock();
        if (c) {
            return NULL;
        }
        if (gone) {
            return stopped_why();
        }
    }
    return "not added: a bad signature, or MeshCore's manual add / hop limit setting kept it out";
}


extern "C" size_t mesh_channels_snapshot(char names[][MESH_CHANNEL_MAX + 1], size_t max) {
    return mesh_channels_snapshot_wait(names, max, MESH_SET_WAIT_MS);
}

extern "C" size_t mesh_channels_snapshot_wait(char names[][MESH_CHANNEL_MAX + 1], size_t max, uint32_t wait_ms) {
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped()) {
        return 0;
    }
    if (!mesh_lock_within(wait_ms)) {
        return MESH_BUSY;
    }
    size_t         n = 0;
    ChannelDetails details;
    for (int i = 0; i < MAX_GROUP_CHANNELS && n < max; i++) {
        if (node->getChannel(i, details) && details.name[0]) {
            mesh_sanitize(details.name, strnlen(details.name, sizeof(details.name)), names[n], MESH_CHANNEL_MAX + 1);
            n++;
        }
    }
    mesh_unlock();
    return n;
}
