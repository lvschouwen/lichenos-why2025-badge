#pragma once
// The apps' secrets class (#288): which keys of an app's namespace are secrets. Pure, host-tested in
// host_tests/app_secret_test.c; lichen_settings.c is the NVS glue, lichen_app.c and app_ns.c check the declarations.
//
// An app declares one key prefix (lichen_app_t.secret_prefix, "pw" for MeshCore). Its secrets live in its own
// namespace a.<id> (so Clear data and a factory reset erase them with the rest of it, also in an older image) under
// the stored name APP_SECRET_MARK + key: "~pw0123456789ab". Only app_secret_get/set/erase reach them. In the class,
// and so refused by every plain app_settings_* call, never in a backup group or on a keep list:
//  - every key that starts with APP_SECRET_MARK, in every app's namespace (the stored form, and the OS's own "~:"
//    record of the one-time move), whatever the app declared;
//  - every key that starts with the app's own prefix (a plain copy an older image left is not handed out either).
// The prefix is the declaring app's only: the class of a key is decided with the prefix of the namespace's owner, so
// another app's "pw..." key is that app's plain setting, and no app can name another's namespace anyway.

#include "settings_backup.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_SECRET_MARK       '~'
#define APP_SECRET_PREFIX_MAX 8  // characters of a declared prefix, a-z 0-9 _
#define APP_SECRET_KEY_MAX    14 // characters of an app's secret key: the mark makes it NVS's 15
#define APP_SECRET_STORED_MAX 16 // a buffer for the stored name, NUL included
#define APP_SECRET_VALUE_MAX  64 // characters of a value (a Wi-Fi password's length); longer is refused

// A declaration lichen_register_app accepts: NULL (no secrets) or 1..APP_SECRET_PREFIX_MAX of a-z 0-9 _
bool app_secret_prefix_ok(char const *prefix);
// True for a key of the secrets class in a namespace whose app declared prefix (NULL: none). A NULL key is not.
bool app_secret_key_secret(char const *prefix, char const *key);
// The stored name of an app secret: false (out emptied when cap > 0) when prefix is not a valid declaration, key does
// not start with it, is longer than APP_SECRET_KEY_MAX, or cap is under APP_SECRET_STORED_MAX
bool app_secret_stored_name(char const *prefix, char const *key, char *out, size_t cap);
// True when none of an app's n backup keys is in its secrets class (a group with one is refused whole)
bool app_secret_keys_plain(char const *prefix, sb_key_t const *keys, size_t n);

#ifdef __cplusplus
}
#endif
