#pragma once

#include <stdbool.h>
#include <stdint.h>

// The path hash size (#129): bytes per hop in the path of a flood. MeshCore's NodePrefs keeps it as path_hash_mode
// (0 = 1 B, 1 = 2 B, 2 = 3 B). Most of the Dutch network uses 2 B, and 1 B hashes let any repeater matching one byte of a
// hop forward a direct message, so 2 B is the default. Pure logic (host-tested in host_tests/mesh_pathhash_test.c).
#define MESH_PATH_HASH_DEFAULT 2

// One-shot: the first boot of the build that made the size a setting moves a badge still on MeshCore's 1 B default (a
// new badge as well) to 2 B. The flag is kept in the app's settings, so a later explicit choice of 1 B stays.
// Returns the size to switch to, or 0 for nothing to do. *mark_done says the flag is to be written.
static inline uint8_t mesh_pathhash_migrate(bool already_done, uint8_t current_size, bool *mark_done) {
    *mark_done = !already_done;
    return (!already_done && current_size == 1) ? MESH_PATH_HASH_DEFAULT : 0;
}
