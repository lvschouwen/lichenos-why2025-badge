#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Summarises a received MeshCore packet for the LoRa log (#51): routing, payload type, path and, for adverts, the
// sender's name. Follows meshcore/src/Packet.cpp readFrom(). Pure logic, no ESP headers: built into the host tests.

typedef struct {
    uint8_t route; // ROUTE_TYPE_*: 0 transport flood, 1 flood, 2 direct, 3 transport direct
    uint8_t type;  // PAYLOAD_TYPE_*
    uint8_t ver;
    uint8_t path_hash_size;
    uint8_t path_hash_count;
    uint8_t payload_len;
    uint8_t last_hop[3]; // flood only: the hash the last repeater appended (path_hash_size bytes), if any
    bool    is_advert;
    uint8_t adv_type; // ADV_TYPE_*: 0 none, 1 chat, 2 repeater, 3 room, 4 sensor
    char    name[33]; // advert name, printable ASCII only, "" if absent
} mc_hdr_t;

// False if the bytes cannot be a MeshCore packet (bad path encoding, no payload, payload too long)
bool mc_hdr_parse(uint8_t const *pkt, size_t len, mc_hdr_t *out);

// One line such as "flood advert v0 path 2x1 'Rep1' (repeater)"; returns the length written (excluding the NUL)
size_t mc_hdr_format(mc_hdr_t const *h, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
