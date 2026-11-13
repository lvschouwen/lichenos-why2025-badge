#pragma once
// Per-app settings rules (#84 T5). Pure, host-tested in host_tests/app_ns_test.c; lichen_settings.c is the NVS glue.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app's NVS namespace: "a." + its id, so it never meets Lichen's badge_fw or another app's. NVS namespaces are at
// most 15 characters, so an id is 1..13 of a-z 0-9 _. False (out untouched) for any other id or a cap under 16.
bool app_ns_name(char const *id, char *out, size_t cap);
// True when ns is exactly the namespace of app id (a key group an app registers may only name its own)
bool app_ns_owns(char const *id, char const *ns);
// The apps' declared keep-over-reset keys (lichen_app_t.keep_keys, NULL-terminated), passed one by one to add(ns, key) with
// ns = app_ns_name(id). A key of the app's secrets class (secret_prefix, app_secret.h) is not passed on. Returns how many
// were not accepted (a bad id counts them all, at least 1 when keys exist)
size_t app_ns_declare_keep(char const *id, char const *secret_prefix, char const *const *keys,
                           bool (*add)(char const *ns, char const *key));

#ifdef __cplusplus
}
#endif
