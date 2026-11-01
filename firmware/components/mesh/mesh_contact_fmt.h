#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A contact's texts for the Contacts tab, its details screen and the console (#85, #49). Pure logic, no ESP headers.

// ContactInfo.out_path_len as MeshCore (v1.9 and later) packs it: the hop count in bits 0-5, the path hash size minus
// one in bits 6-7 (Packet::getPathHashCount / getPathHashSize). 0xFF (OUT_PATH_UNKNOWN) means no path: flood.
#define MESH_PATH_FLOOD 0xFF
typedef struct {
    bool    flood;
    uint8_t hops;      // hashes in the path, 0 = a direct neighbour
    uint8_t hash_size; // bytes per hash, 1-4
} mesh_path_t;
mesh_path_t mesh_path_decode(uint8_t path_len);
// "flood", "direct", "1 hop", "N hops"
void mesh_format_hops(uint8_t path_len, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
