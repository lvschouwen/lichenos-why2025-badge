#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The default flood scope (#163): the region name MeshCore's companion keeps in its prefs (default_scope_name, 30
// characters at most; the key is SHA-256 of "#" + name, like a hashtag channel's). No name = "None (everywhere)": the
// adverts and messages flood without a region, as before. Pure logic, no ESP headers (host_tests/mesh_scope_test.c).

#define MESH_SCOPE_MAX 30

// NULL when the input is a scope: out is the region name, lowercased, with a leading '#' and surrounding spaces gone, or
// "" for None (empty input). Else the reason (out is ""). A name takes letters, digits and '-' (accented letters too);
// MeshCore also lets '$' mark a private region, but that needs a stored key the badge has no way to enter.
char const *mesh_scope_parse(char const *in, char *out, size_t cap);
// What the Settings row shows: "None (everywhere)" for no name, else the name
void mesh_scope_text(char const *name, char *out, size_t cap);
// The question before a region is set (not asked for None)
void mesh_scope_ask_text(char const *name, char *out, size_t cap);
// The text SHA-256 turns into the region key: "#" + name
void mesh_scope_key_text(char const *name, char *out, size_t cap);

// The console scope (#290): every flood a console line starts (send, dm, remote, telemetry, trace, discover) goes out
// under a region of its own, so Claude and the agents never flood further than the bench allows; the screens keep the
// default scope. Unlike the default, an empty console scope is no "None (everywhere)": the console flood is refused.
#define MESH_SCOPE_CONSOLE_UNSET "console scope not set: fw mesh scope console <region> YES"
typedef enum { MESH_SCOPE_USE_DEFAULT, MESH_SCOPE_USE_CONSOLE, MESH_SCOPE_USE_REFUSE } mesh_scope_use_t;
// Which scope a flood takes: a screen's always the default, a console line's the console scope, refused when unset
mesh_scope_use_t mesh_scope_choose(bool from_console, char const *console_name);
// "fw mesh scope console <region> YES": the text after "console". NULL when out holds the region (mesh_scope_parse's
// rules, never empty), else why not (out is "").
char const *mesh_scope_console_arg(char const *args, char *out, size_t cap);
// What "fw mesh scope" prints: "default <name|None> | console <name|not set>" (tools/badge.sh reads the console part)
void mesh_scope_status_text(char const *default_name, char const *console_name, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
