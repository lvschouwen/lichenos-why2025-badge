// Which key a channel name may carry (#451): see channel_key.h
#include "channel_key.h"

#include "mesh_hist_crypto.h"

#include <ctype.h>
#include <string.h>

#define NAME_MAX_BYTES 32 // MeshCore's ChannelDetails name, terminator included

char const CHANNEL_WRONG_KEY[] =
    "A #name needs that hashtag's key; a channel with its own key has a name without the #";

bool channel_hashtag_key(char const *name, uint8_t out[CHANNEL_HASHTAG_KEY_LEN]) {
    char    lower[NAME_MAX_BYTES];
    uint8_t digest[32];
    size_t  n = name ? strnlen(name, sizeof(lower)) : sizeof(lower);
    memset(out, 0, CHANNEL_HASHTAG_KEY_LEN);
    if (n >= sizeof(lower)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    bool ok = mh_sha256(digest, lower, n, NULL, 0);
    if (ok) {
        memcpy(out, digest, CHANNEL_HASHTAG_KEY_LEN);
    }
    mh_wipe(digest, sizeof(digest));
    return ok;
}

char const *channel_key_refusal(char const *name, uint8_t const *secret, size_t secret_len) {
    if (!name || name[0] != '#') {
        return NULL;
    }
    uint8_t tag[CHANNEL_HASHTAG_KEY_LEN];
    bool    same = channel_hashtag_key(name, tag) && secret_len == CHANNEL_HASHTAG_KEY_LEN &&
                memcmp(tag, secret, CHANNEL_HASHTAG_KEY_LEN) == 0;
    mh_wipe(tag, sizeof(tag));
    return same ? NULL : CHANNEL_WRONG_KEY;
}

char const *channel_stored_key_refusal(char const *name, uint8_t const secret[32]) {
    size_t len = CHANNEL_HASHTAG_KEY_LEN;
    for (size_t i = CHANNEL_HASHTAG_KEY_LEN; i < 32; i++) {
        len = secret[i] ? 32 : len;
    }
    return channel_key_refusal(name, secret, len);
}

char const *channel_add_parse(
    char const *name,
    char const *b64_or_null,
    char       *out_name,
    size_t      name_cap,
    uint8_t     out_secret[CHANNEL_SECRET_MAX],
    size_t     *secret_len
) {
    char const *why = channel_parse(name, b64_or_null, out_name, name_cap, out_secret, secret_len);
    if (!why && *secret_len == 0) {
        *secret_len = channel_hashtag_key(out_name, out_secret) ? CHANNEL_HASHTAG_KEY_LEN : 0;
        why         = *secret_len ? NULL : "The hashtag's key could not be made, try again";
    } else if (!why) {
        why = channel_key_refusal(out_name, out_secret, *secret_len);
    }
    if (why) {
        mh_wipe(out_secret, CHANNEL_SECRET_MAX); // a typed key, refused: not left behind
        *secret_len = 0;
    }
    return why;
}
