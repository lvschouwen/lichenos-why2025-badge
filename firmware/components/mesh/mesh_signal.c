#include "mesh_signal.h"

#include "mesh_health.h"

#include <stdio.h>
#include <string.h>

void sig_init(sig_table_t *t) {
    memset(t, 0, sizeof(*t));
}

static sig_contact_t *find(sig_table_t *t, uint8_t const *prefix) {
    for (size_t i = 0; i < SIG_CONTACTS; i++) {
        if (t->c[i].used && memcmp(t->c[i].prefix, prefix, SIG_PREFIX) == 0) {
            return &t->c[i];
        }
    }
    return NULL;
}

// A free slot, else the contact heard longest ago
static sig_contact_t *slot_for_new(sig_table_t *t) {
    sig_contact_t *pick = &t->c[0];
    for (size_t i = 0; i < SIG_CONTACTS; i++) {
        if (!t->c[i].used) {
            return &t->c[i];
        }
        if (t->c[i].order < pick->order) {
            pick = &t->c[i];
        }
    }
    return pick;
}

static void keep_path(sig_contact_t *c, uint8_t path_len, uint8_t const *path, uint32_t at_s) {
    uint8_t size  = (uint8_t)((path_len >> 6) + 1);
    uint8_t hops  = path_len & 63;
    if (size > 3 || (size_t)hops * size > 64) {
        return; // not a valid path_len (MeshCore's isValidPathLen): keep the last good one
    }
    size_t bytes = (size_t)hops * size;
    if (bytes > SIG_PATH_MAX) {
        bytes = (SIG_PATH_MAX / size) * size; // whole hashes only
    }
    c->have_path      = true;
    c->path_hops      = hops;
    c->path_hash_size = size;
    c->path_bytes     = (uint8_t)bytes;
    c->path_at_s      = at_s;
    if (bytes && path) {
        memcpy(c->path, path, bytes);
    }
}

void sig_note(sig_table_t *t, uint8_t const prefix[SIG_PREFIX], sig_kind_t kind, int16_t rssi_x10, int16_t snr_x10,
              uint8_t path_len, uint8_t const *path, uint32_t at_s) {
    sig_contact_t *c = find(t, prefix);
    if (!c) {
        c = slot_for_new(t);
        memset(c, 0, sizeof(*c));
        c->used = true;
        memcpy(c->prefix, prefix, SIG_PREFIX);
    }
    uint8_t hops = path_len & 63;
    c->s[c->next] = (sig_sample_t){.at_s = at_s, .rssi_x10 = rssi_x10, .snr_x10 = snr_x10, .hops = hops, .kind = kind};
    c->next       = (uint8_t)((c->next + 1) % SIG_SAMPLES);
    c->count      = c->count < SIG_SAMPLES ? (uint8_t)(c->count + 1) : SIG_SAMPLES;
    c->heard++;
    c->order = ++t->counter;
    if (kind == SIG_ADVERT) {
        keep_path(c, path_len, path, at_s);
    }
}

bool sig_get(sig_table_t const *t, uint8_t const prefix[SIG_PREFIX], sig_contact_t *out) {
    for (size_t i = 0; i < SIG_CONTACTS; i++) {
        if (t->c[i].used && memcmp(t->c[i].prefix, prefix, SIG_PREFIX) == 0) {
            *out = t->c[i];
            return true;
        }
    }
    return false;
}

size_t sig_samples(sig_contact_t const *c, sig_sample_t *out, size_t max) {
    size_t n = c->count < max ? c->count : max;
    // the newest n, oldest first: the newest is at next - 1
    for (size_t i = 0; i < n; i++) {
        out[i] = c->s[(c->next + SIG_SAMPLES - n + i) % SIG_SAMPLES];
    }
    return n;
}

int16_t sig_value(sig_sample_t const *s, sig_field_t f) {
    int x10 = f == SIG_RSSI ? s->rssi_x10 : s->snr_x10;
    return (int16_t)(x10 >= 0 ? (x10 + 5) / 10 : -((-x10 + 5) / 10));
}

void sig_range(sig_sample_t const *s, size_t n, sig_field_t f, int *lo, int *hi) {
    int l = 0, h = 0;
    for (size_t i = 0; i < n; i++) {
        int v = sig_value(&s[i], f);
        l     = (i == 0 || v < l) ? v : l;
        h     = (i == 0 || v > h) ? v : h;
    }
    // at least 10 units tall: a steady link is a flat line, not a cliff
    while (h - l < 10) {
        h++;
        if (h - l < 10) {
            l--;
        }
    }
    *lo = l;
    *hi = h;
}

char const *sig_kind_name(uint8_t kind) {
    return kind == SIG_ADVERT ? "advert" : "message";
}

void sig_hops_text(uint8_t hops, char *out, size_t cap) {
    if (hops == 0) {
        snprintf(out, cap, "direct");
    } else {
        snprintf(out, cap, "%u hop%s", (unsigned)hops, hops == 1 ? "" : "s");
    }
}

void sig_sample_text(sig_sample_t const *s, char const *age_text, char *out, size_t cap) {
    char rssi[16], snr[16], hops[16];
    mh_fmt_dbm(s->rssi_x10, rssi, sizeof(rssi));
    mh_fmt_db(s->snr_x10, snr, sizeof(snr));
    sig_hops_text(s->hops, hops, sizeof(hops));
    snprintf(out, cap, "%s, %s SNR, %s, %s, %s", rssi, snr, hops, sig_kind_name(s->kind), age_text);
}

void sig_heard_text(uint32_t heard, char *out, size_t cap) {
    if (heard == 1) {
        snprintf(out, cap, "Heard once since start");
    } else {
        snprintf(out, cap, "Heard %lu times since start", (unsigned long)heard);
    }
}

void sig_path_text(sig_contact_t const *c, char *out, size_t cap) {
    if (!c->have_path) {
        snprintf(out, cap, "no advert heard");
        return;
    }
    if (c->path_hops == 0) {
        snprintf(out, cap, "direct (no hops)");
        return;
    }
    size_t n = (size_t)snprintf(out, cap, "%u hop%s: ", (unsigned)c->path_hops, c->path_hops == 1 ? "" : "s");
    size_t shown = c->path_bytes / c->path_hash_size;
    for (size_t h = 0; h < shown && n < cap; h++) {
        n += (size_t)snprintf(out + n, cap - n, "%s", h ? "," : "");
        for (uint8_t b = 0; b < c->path_hash_size && n < cap; b++) {
            n += (size_t)snprintf(out + n, cap - n, "%02x", c->path[h * c->path_hash_size + b]);
        }
    }
    if (shown < c->path_hops && n < cap) {
        snprintf(out + n, cap - n, ",...");
    }
}
