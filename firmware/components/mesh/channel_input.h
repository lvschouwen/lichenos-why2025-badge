#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// What the console and the screen accept as a new channel (#67, #65 review). Pure logic, no ESP headers
// (host_tests/channel_input_test.c).

#define CHANNEL_SECRET_MAX 32
#define CHANNEL_SLOTS      40 // MAX_GROUP_CHANNELS in the mesh component's build

// NULL when the input makes a channel, else the reason. The name is trimmed and at most 31 bytes (out_name, cap >= 32).
// Without a key (NULL or "") it must be a #hashtag: no spaces, lowercased into out_name, *secret_len = 0 (the key is
// SHA-256 of out_name, MeshCore's rule, done by the caller). With a key, the whole key must be base64 of exactly 16 or
// 32 bytes (padded or not), decoded into out_secret.
char const *channel_parse(char const *name, char const *b64_or_null, char *out_name, size_t name_cap,
                          uint8_t out_secret[CHANNEL_SECRET_MAX], size_t *secret_len);
// A channel's new name (#54): NULL when it can be one, else why not. Trimmed, at most 31 bytes, into out (cap >= 32); a
// #hashtag has no spaces and is lowercased, as channel_parse. *hashtag tells the caller to check that the channel's key
// is that hashtag's key: renaming never changes a key, and a #name with another key would never meet that hashtag.
char const *channel_rename_parse(char const *new_name, char *out, size_t cap, bool *hashtag);
// Two channel names are the same channel to the user: compared without case
bool channel_same_name(char const *a, char const *b);
// How well a name typed on the console names a stored channel (#101): CHANNEL_MATCH_EXACT for the same name without
// case, CHANNEL_MATCH_HASHTAG when only the stored name's leading '#' is left out ("badgetest" for "#badgetest"), else
// CHANNEL_MATCH_NONE. Callers take an exact match over a hashtag one, so a keyed "badgetest" is never shadowed.
enum { CHANNEL_MATCH_NONE, CHANNEL_MATCH_HASHTAG, CHANNEL_MATCH_EXACT };
int channel_name_match(char const *stored, char const *typed);

// NULL when the channel in slot `index` (name as stored, NULL or "" for an empty slot) may be removed, else why not:
// slot 0 is Public (MeshCore's default channel), and an empty or missing slot has nothing to remove
char const *channel_removable(int index, char const *name);

#ifdef __cplusplus
}
#endif
