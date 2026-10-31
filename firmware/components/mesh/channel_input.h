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
// Two channel names are the same channel to the user: compared without case
bool channel_same_name(char const *a, char const *b);

// NULL when the channel in slot `index` (name as stored, NULL or "" for an empty slot) may be removed, else why not:
// slot 0 is Public (MeshCore's default channel), and an empty or missing slot has nothing to remove
char const *channel_removable(int index, char const *name);

#ifdef __cplusplus
}
#endif
