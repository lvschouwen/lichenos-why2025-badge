#pragma once

#include <stdbool.h>
#include <stdint.h>

// The path hash size (#129): bytes per hop in the path of a flood. MeshCore's NodePrefs keeps it as path_hash_mode
// (0 = 1 B, 1 = 2 B, 2 = 3 B). Most of the Dutch network uses 2 B, and 1 B hashes let any repeater matching one byte of a
// hop forward a direct message, so 2 B is the default.
#define MESH_PATH_HASH_DEFAULT 2
