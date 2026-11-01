#include "mesh_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR // host tests
#endif

// A table of chats, each with a ring of its messages (oldest overwritten)
typedef struct {
    mesh_conv_t pub;
    mesh_msg_t *ring;
    size_t      head;     // index of the oldest
    bool        used;
} conv_t;

EXT_RAM_BSS_ATTR static conv_t convs[MESH_CONV_MAX]; // 17 KB: PSRAM on the badge (#107)
static size_t   ring_cap;
static uint32_t generation, own_changes, next_seq, dropped;
static void *(*ring_alloc)(size_t, size_t);
static void (*warn_hook)(char const *);
static uint32_t (*clock_hook)(void);

void mesh_store_set_clock(uint32_t (*epoch_now)(void)) {
    clock_hook = epoch_now;
}

uint32_t mesh_store_last_seq(void) {
    return next_seq;
}

void mesh_store_set_hooks(void *(*alloc_ring)(size_t, size_t), void (*warn)(char const *)) {
    ring_alloc = alloc_ring;
    warn_hook  = warn;
}

uint32_t mesh_store_dropped(void) {
    return dropped;
}

static void drop(char const *why) {
    dropped++;
    if (warn_hook) {
        warn_hook(why);
    }
}

bool mesh_store_init(size_t chat_keep) {
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        free(convs[i].ring);
    }
    memset(convs, 0, sizeof(convs));
    ring_cap = chat_keep;
    dropped  = 0;
    generation++;
    return chat_keep > 0;
}

void mesh_chat_key_channel(mesh_chat_key_t *key, char const *name) {
    char clean[MESH_CHANNEL_MAX + 1];
    memset(key, 0, sizeof(*key));
    key->kind = MESH_CHAT_CHANNEL;
    mesh_sanitize(name, strlen(name), clean, sizeof(clean));
    for (size_t i = 0; clean[i]; i++) {
        key->chan[i] = (clean[i] >= 'A' && clean[i] <= 'Z') ? (char)(clean[i] + 32) : clean[i];
    }
}

void mesh_chat_key_dm(mesh_chat_key_t *key, uint8_t const peer[6]) {
    memset(key, 0, sizeof(*key));
    key->kind = MESH_CHAT_DM;
    memcpy(key->id, peer, 6);
}

static bool key_same(mesh_chat_key_t const *a, mesh_chat_key_t const *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static conv_t *find_conv(mesh_chat_key_t const *key) {
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        if (convs[i].used && key_same(&convs[i].pub.key, key)) {
            return &convs[i];
        }
    }
    return NULL;
}

// A free slot, else the DM chat quiet for longest (its slot is taken over once the new ring exists); NULL when the table
// holds only channels
static conv_t *pick_slot(void) {
    conv_t *victim = NULL;
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        if (!convs[i].used) {
            return &convs[i];
        }
        if (convs[i].pub.key.kind == MESH_CHAT_DM && (!victim || convs[i].pub.last_seq < victim->pub.last_seq)) {
            victim = &convs[i];
        }
    }
    return victim;
}

// A new chat: its ring is allocated before any chat is evicted, so a failure costs the new message only
static conv_t *new_conv(mesh_chat_key_t const *key) {
    conv_t *slot = pick_slot();
    if (!slot) {
        drop("chat table full of channels");
        return NULL;
    }
    mesh_msg_t *ring = ring_alloc ? ring_alloc(ring_cap, sizeof(*ring)) : calloc(ring_cap, sizeof(*ring));
    if (!ring) {
        drop("no memory for a chat ring");
        return NULL;
    }
    free(slot->ring);
    memset(slot, 0, sizeof(*slot));
    slot->ring    = ring;
    slot->used    = true;
    slot->pub.key = *key;
    return slot;
}

// Length of the UTF-8 sequence starting at in[0], or 0 when it isn't a valid one within len bytes. Strict, as RFC 3629:
// no overlongs, no surrogates (U+D800..U+DFFF), nothing above U+10FFFF.
static size_t utf8_sequence(unsigned char const *in, size_t len) {
    size_t        need;
    unsigned char lo = 0x80, hi = 0xBF; // allowed range of the second byte
    if (in[0] < 0x80) {
        return 1;
    } else if (in[0] >= 0xC2 && in[0] <= 0xDF) {
        need = 2;
    } else if (in[0] >= 0xE0 && in[0] <= 0xEF) {
        need = 3;
        lo   = in[0] == 0xE0 ? 0xA0 : 0x80;
        hi   = in[0] == 0xED ? 0x9F : 0xBF;
    } else if (in[0] >= 0xF0 && in[0] <= 0xF4) {
        need = 4;
        lo   = in[0] == 0xF0 ? 0x90 : 0x80;
        hi   = in[0] == 0xF4 ? 0x8F : 0xBF;
    } else {
        return 0; // a continuation byte, an overlong lead or out of range
    }
    if (need > len || in[1] < lo || in[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < need; i++) {
        if ((in[i] & 0xC0) != 0x80) {
            return 0;
        }
    }
    return need;
}

// C0 controls, DEL and the C1 controls (U+0080..U+009F, two bytes C2 80..C2 9F)
static bool is_control(unsigned char const *in, size_t seq) {
    return (seq == 1 && (in[0] < 0x20 || in[0] == 0x7F)) || (seq == 2 && in[0] == 0xC2 && in[1] < 0xA0);
}

size_t mesh_sanitize(char const *in, size_t in_len, char *out, size_t cap) {
    unsigned char const *p   = (unsigned char const *)in;
    size_t               pos = 0, n = 0;
    if (cap == 0) {
        return 0;
    }
    while (pos < in_len && p[pos] != '\0') {
        size_t seq = utf8_sequence(p + pos, in_len - pos);
        bool   ctl = seq && is_control(p + pos, seq);
        size_t put = seq && !ctl ? seq : 1;
        if (n + put > cap - 1) {
            break; // never cut a character in half
        }
        if (seq == 0) {
            out[n] = '?';
        } else if (ctl) {
            out[n] = ' ';
        } else {
            memcpy(out + n, p + pos, seq);
        }
        n += put;
        pos += seq ? seq : 1;
    }
    out[n] = '\0';
    return n;
}

static mesh_msg_t *conv_at(conv_t const *cv, size_t i) { // 0 = oldest
    return &cv->ring[(cv->head + i) % ring_cap];
}

// Appends a message to its chat (created on the first one); NULL when out of memory or the table is full of channels.
// The caller fills in the rest (id, dm, peer...) and the unread count.
static mesh_msg_t *push(mesh_chat_key_t const *key, char const *title, char const *channel, char const *text,
                        int16_t rssi_x10, uint32_t heard_s) {
    if (!ring_cap) {
        return NULL;
    }
    conv_t *cv = find_conv(key);
    if (!cv) {
        cv = new_conv(key);
        if (!cv) {
            return NULL;
        }
    }
    size_t slot = (cv->head + cv->pub.count) % ring_cap;
    if (cv->pub.count == ring_cap) {
        cv->head = (cv->head + 1) % ring_cap; // overwrite the oldest
    } else {
        cv->pub.count++;
    }
    mesh_msg_t *m = &cv->ring[slot];
    memset(m, 0, sizeof(*m));
    mesh_sanitize(channel, strlen(channel), m->channel, sizeof(m->channel));
    mesh_sanitize(text, strlen(text), m->text, sizeof(m->text));
    m->rssi_x10 = rssi_x10;
    m->heard_s  = heard_s;
    m->seq      = ++next_seq;
    m->epoch    = clock_hook ? clock_hook() : 0;
    if (!cv->pub.title[0] || strcmp(title, "@?") != 0) { // an unknown contact name does not replace a good one
        mesh_sanitize(title, strlen(title), cv->pub.title, sizeof(cv->pub.title));
    }
    memcpy(cv->pub.last, m->text, sizeof(cv->pub.last));
    cv->pub.last_heard_s = heard_s;
    cv->pub.last_outgoing = 0;
    cv->pub.last_loaded   = 0;
    cv->pub.last_epoch    = m->epoch;
    cv->pub.last_seq      = m->seq;
    return m;
}

// The chat's newest message changed (a loaded one): the list row follows it
static void conv_take_last(conv_t *cv) {
    mesh_msg_t const *m   = conv_at(cv, cv->pub.count - 1);
    memcpy(cv->pub.last, m->text, sizeof(cv->pub.last));
    cv->pub.last_heard_s  = m->heard_s;
    cv->pub.last_outgoing = m->outgoing;
    cv->pub.last_loaded   = m->loaded;
    cv->pub.last_epoch    = m->epoch;
    cv->pub.last_seq      = m->seq;
}

// Where a loaded message goes in its chat's ring (kept in seq order): the index after the last older message; *same
// when the message before that index has this very seq
static size_t loaded_pos(conv_t const *cv, uint32_t seq, bool *same) {
    size_t pos = cv->pub.count;
    while (pos > 0 && conv_at(cv, pos - 1)->seq > seq) {
        pos--;
    }
    *same = pos > 0 && conv_at(cv, pos - 1)->seq == seq;
    return pos;
}

bool mesh_store_add_loaded(mesh_chat_key_t const *key, char const *title, mesh_msg_t const *in) {
    // The counter passes every seq in the journal, kept or not: a new message never reuses one
    next_seq = in->seq > next_seq ? in->seq : next_seq;
    if (!ring_cap) {
        return false;
    }
    conv_t *cv = find_conv(key);
    if (!cv && !(cv = new_conv(key))) {
        return false;
    }
    if (!cv->pub.title[0] || strcmp(title, "@?") != 0) {
        mesh_sanitize(title, strlen(title), cv->pub.title, sizeof(cv->pub.title));
    }
    mesh_msg_t m = *in;
    mesh_sanitize(in->channel, sizeof(in->channel), m.channel, sizeof(m.channel));
    mesh_sanitize(in->text, sizeof(in->text), m.text, sizeof(m.text));
    m.loaded  = 1;
    m.heard_s = 0;
    m.id      = 0;
    bool   same;
    size_t pos = loaded_pos(cv, m.seq, &same);
    if (same) {
        *conv_at(cv, pos - 1) = m;
    } else {
        if (cv->pub.count == ring_cap) {
            if (pos == 0) {
                return false; // older than everything the full ring keeps
            }
            cv->head = (cv->head + 1) % ring_cap; // the oldest goes
            cv->pub.count--;
            pos--;
        }
        for (size_t i = cv->pub.count; i > pos; i--) {
            *conv_at(cv, i) = *conv_at(cv, i - 1);
        }
        cv->pub.count++;
        *conv_at(cv, pos) = m;
    }
    conv_take_last(cv);
    generation++;
    return true;
}

bool mesh_store_add_message(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s) {
    mesh_chat_key_t key;
    mesh_chat_key_channel(&key, channel);
    if (!push(&key, channel, channel, text, rssi_x10, heard_s)) {
        return false;
    }
    conv_t *cv = find_conv(&key);
    if (cv->pub.unread < UINT16_MAX) {
        cv->pub.unread++;
    }
    generation++;
    return true;
}

bool mesh_store_add_dm(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s, uint8_t const peer[6]) {
    mesh_chat_key_t key;
    mesh_chat_key_dm(&key, peer);
    mesh_msg_t *m = push(&key, channel, channel, text, rssi_x10, heard_s);
    if (!m) {
        return false;
    }
    m->dm = 1;
    memcpy(m->peer, peer, 6);
    conv_t *cv = find_conv(&key);
    if (cv->pub.unread < UINT16_MAX) {
        cv->pub.unread++;
    }
    generation++;
    return true;
}

// Our own message: not unread, so it moves only the view generation
static void mark_outgoing(mesh_chat_key_t const *key, mesh_msg_t *m, uint32_t id) {
    m->id       = id;
    m->outgoing = 1;
    find_conv(key)->pub.last_outgoing = 1;
    own_changes++;
}

bool mesh_store_add_outgoing(char const *channel, char const *text, uint32_t id, uint32_t heard_s) {
    mesh_chat_key_t key;
    mesh_chat_key_channel(&key, channel);
    mesh_msg_t *m = push(&key, channel, channel, text, 0, heard_s);
    if (!m) {
        return false;
    }
    mark_outgoing(&key, m, id);
    return true;
}

bool mesh_store_add_outgoing_dm(char const *channel, char const *text, uint32_t id, uint32_t heard_s,
                                uint8_t const peer[6]) {
    mesh_chat_key_t key;
    mesh_chat_key_dm(&key, peer);
    mesh_msg_t *m = push(&key, channel, channel, text, 0, heard_s);
    if (!m) {
        return false;
    }
    m->dm = 1;
    memcpy(m->peer, peer, 6);
    mark_outgoing(&key, m, id);
    return true;
}

bool mesh_store_update_out(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms) {
    for (size_t c = 0; id && c < MESH_CONV_MAX; c++) {
        conv_t const *cv = &convs[c];
        for (size_t i = cv->used ? cv->pub.count : 0; i > 0; i--) {
            mesh_msg_t *m = conv_at(cv, i - 1);
            if (!m->outgoing || m->id != id) {
                continue;
            }
            if (m->state == state && m->repeats == repeats && m->attempt == attempt && m->trip_ms == trip_ms) {
                return false;
            }
            m->state   = state;
            m->repeats = repeats;
            m->attempt = attempt;
            m->trip_ms = trip_ms;
            own_changes++;
            return true;
        }
    }
    return false;
}

size_t mesh_store_message_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        n += convs[i].used ? convs[i].pub.count : 0;
    }
    return n;
}

// Walks all chats from their newest message back: the next-newest message overall, NULL at the end
static mesh_msg_t const *merge_next(size_t left[MESH_CONV_MAX]) {
    size_t best = MESH_CONV_MAX;
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        if (left[i] && (best == MESH_CONV_MAX || conv_at(&convs[i], left[i] - 1)->seq > conv_at(&convs[best], left[best] - 1)->seq)) {
            best = i;
        }
    }
    return best == MESH_CONV_MAX ? NULL : conv_at(&convs[best], --left[best]);
}

static void merge_start(size_t left[MESH_CONV_MAX]) {
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        left[i] = convs[i].used ? convs[i].pub.count : 0;
    }
}

size_t mesh_store_recent(mesh_msg_t *out, size_t max) {
    size_t left[MESH_CONV_MAX], total = mesh_store_message_count();
    size_t n = total < max ? total : max;
    merge_start(left);
    for (size_t k = n; k > 0; k--) {
        out[k - 1] = *merge_next(left);
    }
    return n;
}

bool mesh_store_message(size_t index, mesh_msg_t *out) {
    size_t left[MESH_CONV_MAX], total = mesh_store_message_count();
    if (index >= total) {
        return false;
    }
    merge_start(left);
    for (size_t skip = total - 1 - index;; skip--) {
        mesh_msg_t const *m = merge_next(left);
        if (!skip) {
            *out = *m;
            return true;
        }
    }
}

size_t mesh_store_chat(mesh_chat_key_t const *key, mesh_msg_t *out, size_t max, bool mark_read) {
    conv_t *cv = find_conv(key);
    if (!cv) {
        return 0;
    }
    size_t n     = cv->pub.count < max ? cv->pub.count : max;
    size_t first = cv->pub.count - n;
    for (size_t i = 0; i < n; i++) {
        out[i] = *conv_at(cv, first + i);
    }
    if (mark_read) {
        cv->pub.unread = 0;
    }
    return n;
}

size_t mesh_store_convs(mesh_conv_t *out, size_t max) {
    size_t n = 0;
    for (size_t i = 0; i < MESH_CONV_MAX && n < max; i++) {
        if (convs[i].used) {
            out[n++] = convs[i].pub;
        }
    }
    return n;
}

uint32_t mesh_store_unread_total(void) {
    uint32_t total = 0;
    for (size_t i = 0; i < MESH_CONV_MAX; i++) {
        total += convs[i].used ? convs[i].pub.unread : 0;
    }
    return total;
}

uint32_t mesh_store_generation(void) {
    return generation;
}

uint32_t mesh_store_view_generation(void) {
    return generation + own_changes;
}

void mesh_conv_sort(mesh_conv_t *convs, size_t n) {
    for (size_t i = 1; i < n; i++) {
        mesh_conv_t cur = convs[i];
        size_t      j   = i;
        // cur goes before convs[j-1] when it has messages and the other has none or an older newest message
        while (j > 0 && cur.count > 0 && (convs[j - 1].count == 0 || convs[j - 1].last_seq < cur.last_seq)) {
            convs[j] = convs[j - 1];
            j--;
        }
        convs[j] = cur;
    }
}

static bool channel_configured(mesh_conv_t const *c, char const names[][MESH_CHANNEL_MAX + 1], size_t name_count) {
    for (size_t i = 0; i < name_count; i++) {
        mesh_chat_key_t k;
        mesh_chat_key_channel(&k, names[i]);
        if (key_same(&k, &c->key)) {
            return true;
        }
    }
    return false;
}

size_t mesh_conv_merge_channels(mesh_conv_t *convs, size_t n, size_t max, char const names[][MESH_CHANNEL_MAX + 1],
                                size_t name_count) {
    if (name_count == 0) {
        return n < max ? n : max;
    }
    size_t kept = 0;
    for (size_t i = 0; i < n && kept < max; i++) {
        if (convs[i].key.kind != MESH_CHAT_CHANNEL || channel_configured(&convs[i], names, name_count)) {
            convs[kept++] = convs[i];
        }
    }
    for (size_t i = 0; i < name_count && kept < max; i++) {
        mesh_chat_key_t k;
        bool            have = false;
        mesh_chat_key_channel(&k, names[i]);
        for (size_t j = 0; j < kept && !have; j++) {
            have = key_same(&k, &convs[j].key);
        }
        if (!have) {
            memset(&convs[kept], 0, sizeof(convs[kept]));
            convs[kept].key = k;
            mesh_sanitize(names[i], strlen(names[i]), convs[kept].title, sizeof(convs[kept].title));
            kept++;
        }
    }
    return kept;
}
