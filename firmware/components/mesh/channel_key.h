#pragma once

#include "channel_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Which key a channel name may carry (#451): the one decision for add, rename and send. No ESP headers; the hash is
// mesh_hist_crypto.h's (host_tests/channel_key_test.c). A #name is MeshCore's hashtag channel and nodes, the bench
// guard and the on-air allow list all trust it by name: under that name the badge holds and sends with only the key
// derived from it, never another (a foreign key would send another channel's traffic under a trusted name).

#define CHANNEL_HASHTAG_KEY_LEN 16
// The reply when a #name would carry another key (add and rename, the same text)
extern char const CHANNEL_WRONG_KEY[];

// The key MeshCore's apps give a #hashtag channel: the first 16 bytes of SHA-256 of the name, '#' included, lowercased
// here as they do. false (out zeroed) when the name is too long or the hash failed.
bool        channel_hashtag_key(char const *name, uint8_t out[CHANNEL_HASHTAG_KEY_LEN]);
// NULL when a channel named `name` may carry this key (secret_len bytes, 16 or 32), else why not. A name without '#'
// takes any key (unchanged); a #name only its hashtag's 16-byte key. A failed hash refuses: nothing is guessed.
char const *channel_key_refusal(char const *name, uint8_t const *secret, size_t secret_len);
// The same for a stored channel: MeshCore keeps 32 bytes, a 16-byte key has zeroes after it (mesh_share.h)
char const *channel_stored_key_refusal(char const *name, uint8_t const secret[32]);
// What mesh_channel_add takes: channel_parse's rules, then the key. Without one a #hashtag's own (16 bytes), a typed
// one only when the name may carry it (channel_key_refusal). NULL with out_secret and *secret_len set, else why not
// (out_secret zeroed, *secret_len 0).
char const *channel_add_parse(
    char const *name,
    char const *b64_or_null,
    char       *out_name,
    size_t      name_cap,
    uint8_t     out_secret[CHANNEL_SECRET_MAX],
    size_t     *secret_len
);

#ifdef __cplusplus
}
#endif
