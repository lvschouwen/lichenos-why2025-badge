// What makes a new channel (#67): see channel_input.h
#include "channel_input.h"

#include "mesh_format.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define NAME_MAX_BYTES 31

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

// The whole string, strictly: 22 (16 bytes) or 43 (32 bytes) base64 characters, then only the '=' that belong there
static bool decode_key(char const *b64, uint8_t out[CHANNEL_SECRET_MAX], size_t *out_len) {
    size_t chars = 0;
    while (b64[chars] && b64[chars] != '=') {
        if (b64_value(b64[chars]) < 0) {
            return false;
        }
        chars++;
    }
    size_t pad = strlen(b64 + chars);
    size_t bytes;
    if (chars == 22 && (pad == 0 || pad == 2)) {
        bytes = 16;
    } else if (chars == 43 && (pad == 0 || pad == 1)) {
        bytes = 32;
    } else {
        return false; // also any character after the padding
    }
    for (size_t i = 0; i < pad; i++) {
        if (b64[chars + i] != '=') {
            return false;
        }
    }
    // 4 characters -> 3 bytes; the last group is short, and never writes past `bytes`
    uint32_t acc = 0;
    int      bits = 0;
    size_t   n    = 0;
    for (size_t i = 0; i < chars; i++) {
        acc = (acc << 6) | (uint32_t)b64_value(b64[i]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n < bytes) {
                out[n++] = (uint8_t)(acc >> bits);
            }
        }
    }
    *out_len = n;
    return n == bytes;
}

char const *channel_parse(char const *name, char const *b64_or_null, char *out_name, size_t name_cap,
                          uint8_t out_secret[CHANNEL_SECRET_MAX], size_t *secret_len) {
    char clean[64];
    *secret_len = 0;
    if (name_cap) {
        out_name[0] = '\0';
    }
    if (!mesh_name_clean(name, clean, sizeof(clean))) {
        return "The channel needs a name";
    }
    if (strlen(clean) > NAME_MAX_BYTES || strlen(clean) >= name_cap) {
        return "Channel names are at most 31 characters";
    }
    if (!b64_or_null || !*b64_or_null) {
        if (clean[0] != '#' || !clean[1]) {
            return "A channel without a key must be a #hashtag";
        }
        if (strchr(clean, ' ')) {
            return "A #hashtag has no spaces";
        }
        for (char *p = clean; *p; p++) {
            *p = (char)tolower((unsigned char)*p); // MeshCore's apps derive the key from the lowercase name
        }
    } else {
        uint8_t key[CHANNEL_SECRET_MAX];
        size_t  n = 0;
        if (!decode_key(b64_or_null, key, &n)) {
            return "The key must be base64 of 16 or 32 bytes";
        }
        memcpy(out_secret, key, n);
        *secret_len = n;
    }
    memcpy(out_name, clean, strlen(clean) + 1);
    return NULL;
}

bool channel_same_name(char const *a, char const *b) {
    return strcasecmp(a, b) == 0;
}

char const *channel_removable(int index, char const *name) {
    if (index == 0) {
        return "Public can't be removed";
    }
    if (index < 0 || index >= CHANNEL_SLOTS || !name || !*name) {
        return "No such channel";
    }
    return NULL;
}
