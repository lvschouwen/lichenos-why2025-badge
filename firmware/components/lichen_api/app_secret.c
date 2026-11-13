#include "app_secret.h"

#include <string.h>

bool app_secret_prefix_ok(char const *prefix) {
    if (!prefix) {
        return true;
    }
    size_t n = strlen(prefix);
    if (n == 0 || n > APP_SECRET_PREFIX_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = prefix[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

bool app_secret_key_secret(char const *prefix, char const *key) {
    if (!key) {
        return false;
    }
    if (key[0] == APP_SECRET_MARK) {
        return true;
    }
    // A prefix lichen_register_app would refuse never gets here; an empty one would make every key secret (fail closed)
    return prefix && strncmp(key, prefix, strlen(prefix)) == 0;
}

bool app_secret_stored_name(char const *prefix, char const *key, char *out, size_t cap) {
    if (cap > 0) {
        out[0] = '\0';
    }
    if (!prefix || !key || cap < APP_SECRET_STORED_MAX || !app_secret_prefix_ok(prefix)) {
        return false;
    }
    size_t n = strlen(key);
    if (n > APP_SECRET_KEY_MAX || strncmp(key, prefix, strlen(prefix)) != 0) {
        return false;
    }
    out[0] = APP_SECRET_MARK;
    memcpy(out + 1, key, n + 1);
    return true;
}

bool app_secret_keys_plain(char const *prefix, sb_key_t const *keys, size_t n) {
    for (size_t i = 0; keys && i < n; i++) {
        if (app_secret_key_secret(prefix, keys[i].key)) {
            return false;
        }
    }
    return true;
}
