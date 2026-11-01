#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A contact's texts for the contact lists, its details screen and the console (#85, #49). Pure logic, no ESP headers.

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
void        mesh_format_hops(uint8_t path_len, char *out, size_t cap);

// ---- a contact's details (#49)
// ---------------------------------------------------------------------------------------
#define MESH_CONTACT_PUB_LEN  32
#define MESH_CONTACT_PATH_MAX 64 // MAX_PATH_SIZE
#define MESH_CONTACT_NAME_MAX 32
#define MESH_CONTACT_FAV      0x01 // ContactInfo.flags bit 0 (MeshCore's apps: favourite)

// What the details screen changes; mesh_contact_do() in mesh.h queues them for MyMesh
typedef enum { MESH_CO_RESET_PATH, MESH_CO_FAV_ON, MESH_CO_FAV_OFF, MESH_CO_REMOVE } mesh_contact_op_t;

// One contact, copied under the mesh lock (mesh_contact_detail in mesh.h)
typedef struct {
    uint8_t  pub[MESH_CONTACT_PUB_LEN];
    char     name[MESH_CONTACT_NAME_MAX + 1];
    uint8_t  type, flags, path_len;
    uint8_t  path[MESH_CONTACT_PATH_MAX];
    uint32_t age_s;          // since it was last heard (lastmod), by our clock
    int32_t  lat_e6, lon_e6; // from its advert, 0/0 when it shares none
} mesh_contact_detail_t;

// MeshCore's advert types as the details screen names them: "companion", "repeater", "room", "sensor", "unknown"
char const *mesh_contact_type_name(uint8_t type);
// n bytes as lowercase hex, a space after every group bytes (0: none; the Key line uses 4 bytes = 8 hex digits); cut
// on a whole byte to what fits in cap
void        mesh_hex(uint8_t const *bytes, size_t n, size_t group, char *out, size_t cap);
// "flood", "direct", or "2 hops via a1b2,c3d4 (2-byte hashes)": the hops with the hash of each repeater
void        mesh_format_route(uint8_t path_len, uint8_t const *path, char *out, size_t cap);
// The share link MeshCore's apps read (docs/qr_codes.md):
// meshcore://contact/add?name=<url-encoded>&public_key=<64 hex>&type=<n>. false (out empty) when it does not fit.
#define MESH_URI_MAX 224 // a 32-byte name with every byte escaped: 207
bool mesh_contact_uri(char const *name, uint8_t const pub[MESH_CONTACT_PUB_LEN], uint8_t type, char *out, size_t cap);
// The Contacts order: favourites first, then the most recently heard
bool mesh_contact_before(uint8_t flags_a, uint32_t age_a, uint8_t flags_b, uint32_t age_b);

// The contact lists of the app's grid (#88): Contacts (MESH_VIEW_CONTACTS, #101) the people, so everything but
// repeaters and rooms; Repeaters only repeaters and rooms (favourites first in both, as mesh_contact_before); Heard all
// of them by when they were last heard only, newest first. MESH_VIEW_ALL is every contact (the console's list).
typedef enum { MESH_VIEW_ALL, MESH_VIEW_REPEATERS, MESH_VIEW_HEARD, MESH_VIEW_CONTACTS } mesh_contact_view_t;
#define MESH_ADV_TYPE_REPEATER 2 // MeshCore ADV_TYPE_REPEATER
#define MESH_ADV_TYPE_ROOM     3 // ADV_TYPE_ROOM
// Whether a contact of this MeshCore advert type is in the view
bool mesh_contact_view_keeps(mesh_contact_view_t view, uint8_t type);
// The view's order: true when a comes before b
bool mesh_contact_view_before(mesh_contact_view_t view, uint8_t flags_a, uint32_t age_a, uint8_t flags_b,
                              uint32_t age_b);

// The details screen's lines, always in this order: "Type: ...", "ID: <12 hex>" (what `fw mesh dm` takes), "Key: <64
// hex in groups of 8 digits>", "Last heard: ...", "Route: ...", "Location: ..."
enum {
    MESH_LINE_TYPE,
    MESH_LINE_ID,
    MESH_LINE_KEY,
    MESH_LINE_HEARD,
    MESH_LINE_ROUTE,
    MESH_LINE_LOCATION,
    MESH_CONTACT_LINES
};
#define MESH_LINE_MAX 256
typedef struct {
    char text[MESH_LINE_MAX];
} mesh_contact_line_t;
// Fills up to max lines; returns how many (MESH_CONTACT_LINES when max allows)
size_t mesh_contact_lines(mesh_contact_detail_t const *d, mesh_contact_line_t *out, size_t max);

#ifdef __cplusplus
}
#endif
