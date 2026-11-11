// A settings row's text for one registry key (#332, #333): the applied value, how old it is, and whether it differs from
// what is saved. Pure (host_tests/reg_row_text_test.c); the cached text is lichen_reg_cached_text's.
#pragma once

#include <stddef.h>

// "[<label>: ]<value>[<unit>] (3.2 s)" for a fresh read, "... (stale 4 s)" for an old one, "... (saved <saved>)" when
// the applied value is not the saved one, "[<label>: ]? (<why>)" when the key was never read, "[<label>: ]?" when the
// name is unknown (cached empty or NULL). A stale read never claims "differs": the value may have changed since.
// label, saved and unit may be NULL; saved is the value text as the cached text prints it (no unit).
size_t reg_row_text(char const *label, char const *cached, char const *saved, char const *unit, char *out, size_t cap);
