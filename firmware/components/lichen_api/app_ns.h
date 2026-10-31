#pragma once
// Per-app settings rules (#84 T5). Pure, host-tested in host_tests/app_ns_test.c; lichen_settings.c is the NVS glue.

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app's NVS namespace: "a." + its id, so it never meets Lichen's badge_fw or another app's. NVS namespaces are at
// most 15 characters, so an id is 1..13 of a-z 0-9 _. False (out untouched) for any other id or a cap under 16.
bool app_ns_name(char const *id, char *out, size_t cap);

// A key an earlier firmware kept in badge_fw (MeshCore's mc_name_set): copied into the app's namespace once, only when
// the app has none yet and the old one exists. The old key stays, so a rollback still sees it (erased in #84 T9).
typedef enum { APP_ADOPT_NONE, APP_ADOPT_COPY } app_adopt_t;
app_adopt_t app_adopt_plan(bool have_app, bool have_legacy);

#ifdef __cplusplus
}
#endif
