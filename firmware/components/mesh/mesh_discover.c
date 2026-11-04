#include "mesh_discover.h"

#include "mesh_format.h"
#include "mesh_health.h"

#include <stdio.h>
#include <string.h>

void disc_init(disc_t *d) {
    memset(d, 0, sizeof(*d));
}

uint32_t disc_wait_s(disc_t const *d, uint32_t now_s) {
    if (!d->have_asked) {
        return 0;
    }
    uint32_t since = now_s - d->asked_s;
    return since >= DISC_GAP_S ? 0 : DISC_GAP_S - since;
}

bool disc_start(disc_t *d, uint32_t tag, uint32_t now_s, char const **why) {
    if (d->state == DISC_PENDING || d->state == DISC_LISTENING) {
        *why = "a request is still running";
        return false;
    }
    if (disc_wait_s(d, now_s) > 0) {
        *why = "asked a moment ago";
        return false;
    }
    memset(d->a, 0, sizeof(d->a));
    d->count      = 0;
    d->extra      = 0;
    d->why        = NULL;
    d->tag        = tag;
    d->asked_s    = now_s;
    d->have_asked = true;
    d->state      = DISC_PENDING;
    return true;
}

void disc_request(uint32_t tag, uint8_t out[DISC_REQ_LEN]) {
    out[0] = DISC_REQ_TYPE;
    out[1] = DISC_TYPE_ALL;
    memcpy(&out[2], &tag, 4);
}

void disc_sent(disc_t *d, uint32_t now_s) {
    if (d->state == DISC_PENDING) {
        d->state   = DISC_LISTENING;
        d->asked_s = now_s;
    }
}

void disc_failed(disc_t *d, char const *why) {
    if (d->state == DISC_PENDING) {
        d->state = DISC_FAILED;
        d->why   = why;
    }
}

bool disc_on_payload(disc_t *d, uint8_t const *payload, size_t len, int16_t snr_x10, char const *name, uint32_t now_s) {
    if (d->state != DISC_LISTENING || len < 6 + DISC_PREFIX || (payload[0] & 0xF0) != (DISC_RESP_TYPE & 0xF0)) {
        return false;
    }
    uint32_t tag;
    memcpy(&tag, &payload[2], 4);
    if (tag != d->tag) {
        return false;
    }
    uint8_t const *key = &payload[6];
    for (size_t i = 0; i < d->count; i++) {
        if (memcmp(d->a[i].prefix, key, DISC_PREFIX) == 0) {
            d->a[i].snr_x10      = snr_x10;
            d->a[i].their_snr_x4 = (int8_t)payload[1];
            d->a[i].at_s         = now_s;
            return true;
        }
    }
    if (d->count >= DISC_MAX) {
        d->extra++;
        return true;
    }
    disc_answer_t *a = &d->a[d->count++];
    memcpy(a->prefix, key, DISC_PREFIX);
    a->type         = payload[0] & 0x0F;
    a->snr_x10      = snr_x10;
    a->their_snr_x4 = (int8_t)payload[1];
    a->at_s         = now_s;
    if (name) {
        snprintf(a->name, sizeof(a->name), "%s", name);
    }
    return true;
}

disc_state_t disc_poll(disc_t *d, uint32_t now_s) {
    if (d->state == DISC_LISTENING && now_s - d->asked_s >= DISC_WINDOW_S) {
        d->state = DISC_DONE;
    }
    return d->state;
}

size_t disc_copy(disc_t const *d, disc_answer_t *out, size_t max) {
    size_t n = d->count < max ? d->count : max;
    for (size_t i = 0; i < n; i++) {
        out[i] = d->a[i];
    }
    // newest first: insertion sort by answer time, later entries win a tie (they arrived later)
    for (size_t i = 1; i < n; i++) {
        disc_answer_t t = out[i];
        size_t        j = i;
        while (j > 0 && out[j - 1].at_s <= t.at_s) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = t;
    }
    return n;
}

void disc_status(disc_t const *d, uint32_t now_s, char *out, size_t cap) {
    unsigned n = (unsigned)d->count + d->extra;
    switch (d->state) {
    case DISC_IDLE:
        snprintf(out, cap, "Sends one short request on air. Repeaters in direct range answer.");
        break;
    case DISC_PENDING:
        snprintf(out, cap, "Sending...");
        break;
    case DISC_LISTENING: {
        uint32_t used = now_s - d->asked_s;
        uint32_t left = used >= DISC_WINDOW_S ? 0 : DISC_WINDOW_S - used;
        if (n) {
            snprintf(out, cap, "Listening, %u s left. Heard %u so far.", (unsigned)left, n);
        } else {
            snprintf(out, cap, "Listening, %u s left...", (unsigned)left);
        }
        break;
    }
    case DISC_DONE:
        if (n == 0) {
            snprintf(out, cap, "No answers. Nobody nearby, or out of range.");
        } else if (d->extra) {
            snprintf(out, cap, "Heard %u (showing %u)", n, (unsigned)d->count);
        } else {
            snprintf(out, cap, "Heard %u", n);
        }
        break;
    case DISC_FAILED:
        snprintf(out, cap, "Not sent: %s. Try again.", d->why ? d->why : "unknown reason");
        break;
    }
}

static char const *type_title(uint8_t type) {
    char const *n = mesh_type_name(type);
    return strcmp(n, "?") == 0 ? "Node" : n;
}

void disc_row_title(disc_answer_t const *a, char *out, size_t cap) {
    if (a->name[0]) {
        snprintf(out, cap, "%s", a->name);
        return;
    }
    snprintf(out, cap, "%s %02x%02x%02x%02x", type_title(a->type), a->prefix[0], a->prefix[1], a->prefix[2],
             a->prefix[3]);
}

void disc_row_detail(disc_answer_t const *a, uint32_t now_s, char *out, size_t cap) {
    char snr[16], theirs[16], age[24];
    mh_fmt_db(a->snr_x10, snr, sizeof(snr));
    mh_fmt_db((int16_t)(a->their_snr_x4 * 10 / 4), theirs, sizeof(theirs));
    mesh_format_age(now_s > a->at_s ? now_s - a->at_s : 0, age, sizeof(age));
    snprintf(out, cap, "%s, SNR %s, hears us at %s, %s", type_title(a->type), snr, theirs, age);
}
