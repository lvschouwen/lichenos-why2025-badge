#include "app_ns.h"

#include "app_secret.h"

#include <stdio.h>
#include <string.h>

#define APP_ID_MAX 13 // "a." + id fits NVS's 15-character namespace

bool app_ns_name(char const *id, char *out, size_t cap) {
    if (!id || cap < APP_ID_MAX + 3) {
        return false;
    }
    size_t n = strlen(id);
    if (n == 0 || n > APP_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    snprintf(out, cap, "a.%s", id);
    return true;
}

bool app_ns_owns(char const *id, char const *ns) {
    char want[APP_ID_MAX + 3];
    return ns && app_ns_name(id, want, sizeof(want)) && strcmp(want, ns) == 0;
}

size_t app_ns_declare_keep(char const *id, char const *secret_prefix, char const *const *keys,
                           bool (*add)(char const *ns, char const *key)) {
    size_t failed = 0;
    char   ns[APP_ID_MAX + 3];
    bool   ns_ok = app_ns_name(id, ns, sizeof(ns));
    for (; keys && *keys; keys++) {
        // A secret never survives a reset (#288): one on the keep list is refused like a key that does not fit
        if (!ns_ok || app_secret_key_secret(secret_prefix, *keys) || !add(ns, *keys)) {
            failed++;
        }
    }
    return failed;
}
