#include "mesh_chat.h"

#include "channel_input.h"
#include "list_filter.h"
#include "mesh_format.h"
#include "mesh_frame.h"
#include "mesh_outbox.h"

#include <stdio.h>
#include <string.h>

void mesh_format_trip(uint32_t ms, char *out, size_t cap) {
    if (ms < 1000) {
        snprintf(out, cap, "%u ms", (unsigned)ms);
    } else {
        snprintf(out, cap, "%u.%u s", (unsigned)(ms / 1000), (unsigned)(ms % 1000 / 100));
    }
}

static mesh_status_kind_t dm_status(mesh_msg_t const *m, char *out, size_t cap) {
    char try_[16] = "";
    if (m->attempt > 1) {
        snprintf(try_, sizeof(try_), ", try %u/%u", (unsigned)m->attempt, OB_MAX_ATTEMPTS);
    }
    switch ((mesh_out_state_t)m->state) {
    case MESH_OUT_QUEUED:
        snprintf(out, cap, m->attempt ? "retrying%s" : "queued", try_);
        return MESH_STATUS_PENDING;
    case MESH_OUT_SENT:
    case MESH_OUT_ON_AIR:
    case MESH_OUT_HEARD:
        snprintf(out, cap, "waiting for ACK%s", try_);
        return MESH_STATUS_PENDING;
    case MESH_OUT_DELIVERED: {
        char trip[16];
        mesh_format_trip(m->trip_ms, trip, sizeof(trip));
        snprintf(out, cap, "delivered, %s", trip);
        return MESH_STATUS_OK;
    }
    case MESH_OUT_FAILED:
    default:
        snprintf(out, cap, "not delivered");
        return MESH_STATUS_FAILED;
    }
}

bool mesh_msg_in_dm(mesh_msg_t const *m, uint8_t const peer[6]) {
    return m->dm && memcmp(m->peer, peer, 6) == 0;
}

mesh_status_kind_t mesh_msg_status(mesh_msg_t const *m, char *out, size_t cap) {
    if (cap) {
        out[0] = '\0';
    }
    if (!m->outgoing) {
        return MESH_STATUS_NONE;
    }
    if (m->dm) {
        return dm_status(m, out, cap);
    }
    switch ((mesh_out_state_t)m->state) {
    case MESH_OUT_QUEUED:
    case MESH_OUT_SENT:
        snprintf(out, cap, "queued");
        return MESH_STATUS_PENDING;
    case MESH_OUT_ON_AIR:
        snprintf(out, cap, "on air");
        return MESH_STATUS_PENDING;
    case MESH_OUT_HEARD:
        if (m->repeats > 1) {
            snprintf(out, cap, "heard repeated x%u", (unsigned)m->repeats);
        } else {
            snprintf(out, cap, "heard repeated");
        }
        return MESH_STATUS_OK;
    case MESH_OUT_DELIVERED:
        snprintf(out, cap, "delivered");
        return MESH_STATUS_OK;
    case MESH_OUT_FAILED:
    default:
        snprintf(out, cap, "not sent");
        return MESH_STATUS_FAILED;
    }
}

void mesh_conv_row(mesh_conv_t const *c, uint32_t now_s, uint32_t now_epoch, char *title, size_t title_cap,
                   char *badge, size_t badge_cap, char *sub, size_t sub_cap) {
    snprintf(title, title_cap, "%s", c->title);
    if (c->unread == 0) {
        badge[0] = '\0';
    } else if (c->unread > 99) {
        snprintf(badge, badge_cap, "99+");
    } else {
        snprintf(badge, badge_cap, "%u", (unsigned)c->unread);
    }
    char const *gone = c->key.kind == MESH_CHAT_DM && c->contact_gone ? "contact removed" : NULL;
    if (c->count == 0) {
        snprintf(sub, sub_cap, "%s", gone ? gone : "no messages yet");
        return;
    }
    char age[24];
    mesh_format_msg_age(c->last_loaded, c->last_heard_s, c->last_epoch, now_s, now_epoch, age, sizeof(age));
    // The latest line first, its age after it (#101): "bob: hello · 3 min ago"
    snprintf(sub, sub_cap, "%s%s%s · %s", gone ? gone : "", gone ? " · " : "", c->last, age);
}

char const *mesh_advert_refusal(bool clock_set) {
    return clock_set ? NULL : "clock not set";
}

char const *mesh_clock_hint(bool clock_set, bool approximate) {
    if (!clock_set) {
        return "Clock not set, others see a wrong time    Enter: send    /: search    Esc: back";
    }
    return approximate ? "Clock from mesh contacts, unverified    Enter: send    /: search    Esc: back" : NULL;
}

char const *mesh_advert_note(bool approximate) {
    return approximate ? "approximate clock" : NULL;
}

size_t mesh_compose_status(char const *text, size_t max_bytes, char *out, size_t cap) {
    size_t len  = strlen(text);
    size_t sent = mf_text_cut(text, max_bytes);
    if (sent < len) {
        snprintf(out, cap, "%u / %u bytes, the last %u are cut", (unsigned)len, (unsigned)max_bytes,
                 (unsigned)(len - sent));
    } else {
        snprintf(out, cap, "%u / %u bytes", (unsigned)len, (unsigned)max_bytes);
    }
    return sent;
}

static bool word_in_text(char const *word, void *text) {
    return list_filter_text_has(text, word);
}

size_t mesh_chat_hits(mesh_msg_t const *msgs, size_t n, char const *query, uint16_t *hits, size_t max) {
    if (!query || !query[strspn(query, " ")]) {
        return 0;
    }
    size_t found = 0;
    for (size_t i = 0; i < n && found < max; i++) {
        if (list_filter_match(query, word_in_text, (void *)msgs[i].text)) {
            hits[found++] = (uint16_t)i;
        }
    }
    return found;
}

size_t mesh_chat_hit_step(size_t pos, size_t n_hits, int dir) {
    if (n_hits == 0) {
        return MESH_CHAT_NO_HIT;
    }
    if (pos >= n_hits || dir == 0) {
        return n_hits - 1;
    }
    if (dir < 0) {
        return pos == 0 ? n_hits - 1 : pos - 1;
    }
    return pos + 1 == n_hits ? 0 : pos + 1;
}

size_t mesh_chat_hit_of_seq(mesh_msg_t const *msgs, uint16_t const *hits, size_t n_hits, uint32_t seq) {
    for (size_t i = 0; i < n_hits; i++) {
        if (msgs[hits[i]].seq == seq) {
            return i;
        }
    }
    return MESH_CHAT_NO_HIT;
}

size_t mesh_chat_rehit(mesh_msg_t const *msgs, size_t n, char const *query, uint16_t *hits, size_t max, bool have_prev,
                       uint32_t prev_seq, size_t *pos) {
    size_t n_hits = mesh_chat_hits(msgs, n, query, hits, max);
    *pos          = have_prev ? mesh_chat_hit_of_seq(msgs, hits, n_hits, prev_seq) : MESH_CHAT_NO_HIT;
    if (*pos == MESH_CHAT_NO_HIT) {
        *pos = mesh_chat_hit_step(MESH_CHAT_NO_HIT, n_hits, 0); // the newest hit
    }
    return n_hits;
}

void mesh_chat_search_line(char const *query, size_t pos, size_t n_hits, char *out, size_t cap) {
    if (!query) {
        query = "";
    }
    if (!query[0]) {
        snprintf(out, cap, "Search: _  type to search");
    } else if (n_hits == 0 || pos >= n_hits) {
        snprintf(out, cap, "Search: %s_  no match", query);
    } else {
        snprintf(out, cap, "Search: %s_  (%u of %u)", query, (unsigned)(n_hits - pos), (unsigned)n_hits);
    }
}
