#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sharing channels and contacts as text (#54). Pure logic, no ESP headers (host_tests/mesh_share_test.c).

// ---- channels ------------------------------------------------------------------------------------------------------

#define MESH_CHANNEL_SECRET 32 // MeshCore keeps 32 bytes per channel; a 16-byte key has zeroes after it
// How long a stored channel key is: 16 when bytes 16..31 are all zero (MeshCore's apps only make 16-byte keys, the
// hashtag ones too), else 32
size_t mesh_channel_key_len(uint8_t const secret[MESH_CHANNEL_SECRET]);

// Lowercase hex / padded base64 (what "fw channel add" takes) of n bytes; false (out empty) when it does not fit
#define MESH_KEY_HEX_MAX (MESH_CHANNEL_SECRET * 2 + 1)
#define MESH_KEY_B64_MAX 45 // 32 bytes: 44 characters
bool mesh_hex_text(uint8_t const *bytes, size_t n, char *out, size_t cap);
bool mesh_base64_text(uint8_t const *bytes, size_t n, char *out, size_t cap);

// The link MeshCore's apps read to add a channel (meshcore/docs/qr_codes.md):
// meshcore://channel/add?name=<url-encoded>&secret=<32 hex>. The format only knows 16-byte secrets: a 32-byte key gives
// false (out empty), the caller shows the key itself instead. Also false when it does not fit.
#define MESH_CHANNEL_URI_MAX 200 // a 31-byte name with every byte escaped: 160
bool mesh_channel_uri(char const *name, uint8_t const *secret, size_t key_len, char *out, size_t cap);

// ---- contacts: MeshCore's export / import (a signed advert packet as meshcore://<hex>) ------------------------------

#define MESH_ADVERT_MAX     255 // MAX_TRANS_UNIT
#define MESH_ADVERT_PUB_AT  2   // the public key's offset in a normalised advert
#define MESH_ADVERT_PUB_LEN 32
// A raw advert packet (MeshCore's Packet::writeTo bytes) as it is shared: flood route, no transport codes, no path
// (the path is how it reached us, not part of the signed advert, and would only make the link longer). Returns the
// new length in out (cap >= len), 0 when it is not an advert packet MeshCore would read.
size_t mesh_advert_normalize(uint8_t const *in, size_t len, uint8_t *out, size_t cap);

// "meshcore://" and the hex of a normalised advert; false (out empty) when it does not fit. A 255-byte packet needs
// 522 bytes, a normalised one at most 2 + 184 bytes: 384 with the terminator.
#define MESH_ADVERT_URI_MAX (11 + 2 * (2 + 184) + 1)
bool mesh_advert_uri(uint8_t const *pkt, size_t len, char *out, size_t cap);
// The other way round: the normalised packet in out (cap MESH_ADVERT_MAX), its length, 0 with *why set when the text
// is not an advert link (spaces around it are ignored, the scheme is matched without case)
size_t mesh_advert_uri_parse(char const *text, uint8_t *out, size_t cap, char const **why);

// The console takes at most 95 bytes a line, a link is up to 383: it is pasted in parts. A part starting with
// "meshcore://" starts a new link; the others are added to it (spaces dropped). NULL when taken, else why not (the
// pending text is dropped then).
#define MESH_IMPORT_TEXT_MAX MESH_ADVERT_URI_MAX
typedef struct {
    char   text[MESH_IMPORT_TEXT_MAX];
    size_t len; // 0: nothing pending
} mesh_import_t;
char const *mesh_import_add(mesh_import_t *st, char const *part);

#ifdef __cplusplus
}
#endif
