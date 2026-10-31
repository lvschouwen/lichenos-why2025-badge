#include "mesh_store.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// A ring, oldest overwritten
static mesh_msg_t *messages;
static size_t      msg_cap, msg_head, msg_count; // msg_head = index of the oldest
static uint32_t    generation, own_changes;

bool mesh_store_init(size_t max_messages) {
    free(messages);
    messages = calloc(max_messages, sizeof(*messages));
    msg_cap  = messages ? max_messages : 0;
    msg_head = msg_count = 0;
    generation++;
    return messages != NULL;
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

void mesh_store_add_message(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s) {
    if (!msg_cap) {
        return;
    }
    size_t slot = (msg_head + msg_count) % msg_cap;
    if (msg_count == msg_cap) {
        msg_head = (msg_head + 1) % msg_cap; // overwrite the oldest
    } else {
        msg_count++;
    }
    mesh_msg_t *m = &messages[slot];
    mesh_sanitize(channel, strlen(channel), m->channel, sizeof(m->channel));
    mesh_sanitize(text, strlen(text), m->text, sizeof(m->text));
    m->rssi_x10 = rssi_x10;
    m->heard_s  = heard_s;
    m->id = m->outgoing = m->state = m->repeats = m->dm = m->attempt = 0;
    m->trip_ms = 0;
    memset(m->peer, 0, sizeof(m->peer));
    generation++;
}

void mesh_store_add_outgoing(char const *channel, char const *text, uint32_t id, uint32_t heard_s) {
    if (!msg_cap) {
        return;
    }
    mesh_store_add_message(channel, text, 0, heard_s);
    generation--; // undo: only incoming messages count as unread
    mesh_msg_t *m = &messages[(msg_head + msg_count - 1) % msg_cap];
    m->id         = id;
    m->outgoing   = 1;
    own_changes++;
}

static mesh_msg_t *newest(void) {
    return &messages[(msg_head + msg_count - 1) % msg_cap];
}

void mesh_store_add_dm(char const *channel, char const *text, int16_t rssi_x10, uint32_t heard_s, uint8_t const peer[6]) {
    if (!msg_cap) {
        return;
    }
    mesh_store_add_message(channel, text, rssi_x10, heard_s);
    mesh_msg_t *m = newest();
    m->dm         = 1;
    memcpy(m->peer, peer, sizeof(m->peer));
}

void mesh_store_add_outgoing_dm(char const *channel, char const *text, uint32_t id, uint32_t heard_s,
                                uint8_t const peer[6]) {
    if (!msg_cap) {
        return;
    }
    mesh_store_add_outgoing(channel, text, id, heard_s);
    mesh_msg_t *m = newest();
    m->dm         = 1;
    memcpy(m->peer, peer, sizeof(m->peer));
}

bool mesh_store_update_out(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms) {
    for (size_t i = msg_count; id && i > 0; i--) {
        mesh_msg_t *m = &messages[(msg_head + i - 1) % msg_cap];
        if (m->outgoing && m->id == id) {
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
    return msg_count;
}

bool mesh_store_message(size_t index, mesh_msg_t *out) {
    if (index >= msg_count) {
        return false;
    }
    *out = messages[(msg_head + index) % msg_cap];
    return true;
}

uint32_t mesh_store_generation(void) {
    return generation;
}

uint32_t mesh_store_view_generation(void) {
    return generation + own_changes;
}
