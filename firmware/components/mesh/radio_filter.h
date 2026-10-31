#pragma once

#include <stdbool.h>
#include <stddef.h>

// Type-to-search over MeshCore's region presets (#32): pure logic, host-tested in host_tests/radio_filter_test.c
// (the generic part is lichen's list_filter). A word matches a preset when it is inside the title, inside one of the
// preset's aliases (countries and codes: the EU/UK rows list the EU countries that have no preset of their own), or,
// for digits and dots, the start of the frequency printed as %.3f. Every word of the query must match.
bool radio_filter_match(size_t preset, char const *query);
// How many aliases lines there are (equals radio_preset_count(); a test checks it)
size_t radio_filter_alias_count(void);
