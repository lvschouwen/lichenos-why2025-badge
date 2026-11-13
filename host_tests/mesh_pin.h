// Field-by-field FNV-1a digests of the mesh decoder results for the #212 pins: every field is hashed on its own (an
// enum as its value), so struct padding, enum size or a compiler flag cannot change a pin. Header-only.
#pragma once

#include "mesh_frame.h"

#include <stddef.h>
#include <stdint.h>

static inline uint32_t pin_fnv(uint32_t h, void const *p, size_t n) {
    uint8_t const *b = p;
    while (n--) {
        h = (h ^ *b++) * 16777619u;
    }
    return h;
}

static inline uint32_t pin_u32(uint32_t h, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return pin_fnv(h, b, sizeof(b));
}

// A scalar field (bool, enum, signed or unsigned up to 32 bits) as its value; an array of bytes as its bytes
#define PIN_V(h, x) ((h) = pin_u32((h), (uint32_t)(x)))
#define PIN_A(h, a) ((h) = pin_fnv((h), (a), sizeof(a)))

static inline uint32_t pin_status(uint32_t h, mf_status_t const *s) {
    PIN_V(h, s->bytes);
    PIN_V(h, s->batt_mv);
    PIN_V(h, s->tx_queue);
    PIN_V(h, s->noise_floor);
    PIN_V(h, s->last_rssi);
    PIN_V(h, s->n_recv);
    PIN_V(h, s->n_sent);
    PIN_V(h, s->air_secs);
    PIN_V(h, s->up_secs);
    PIN_V(h, s->sent_flood);
    PIN_V(h, s->sent_direct);
    PIN_V(h, s->recv_flood);
    PIN_V(h, s->recv_direct);
    PIN_V(h, s->err_events);
    PIN_V(h, s->last_snr_q);
    PIN_V(h, s->direct_dups);
    PIN_V(h, s->flood_dups);
    PIN_V(h, s->rx_air_secs);
    PIN_V(h, s->recv_errors);
    return h;
}

static inline uint32_t pin_msg(uint32_t h, mf_msg_t const *m) {
    PIN_V(h, m->kind);
    PIN_V(h, m->code);
    PIN_V(h, m->err);
    PIN_V(h, m->flood);
    PIN_V(h, m->fw_ver);
    PIN_V(h, m->is_channel);
    PIN_V(h, m->ack);
    PIN_V(h, m->est_timeout);
    PIN_V(h, m->trip_ms);
    PIN_A(h, m->pub);
    PIN_A(h, m->name);
    PIN_V(h, m->trace.tag);
    PIN_V(h, m->trace.auth);
    PIN_V(h, m->trace.flags);
    PIN_V(h, m->trace.hash_len);
    PIN_V(h, m->trace.hops);
    PIN_A(h, m->trace.hashes);
    PIN_A(h, m->trace.snr_q);
    PIN_V(h, m->trace.final_snr_q);
    PIN_A(h, m->disc.prefix);
    PIN_V(h, m->disc.out_len);
    PIN_V(h, m->disc.in_len);
    PIN_A(h, m->disc.out_path);
    PIN_A(h, m->disc.in_path);
    PIN_A(h, m->rprefix);
    PIN_V(h, m->admin);
    PIN_V(h, m->acl);
    PIN_V(h, m->fw_level);
    h = pin_status(h, &m->status);
    PIN_V(h, m->telem_len);
    PIN_A(h, m->telem);
    PIN_V(h, m->msg_ok);
    PIN_V(h, m->txt_type);
    PIN_A(h, m->author);
    PIN_A(h, m->text);
    return h;
}
