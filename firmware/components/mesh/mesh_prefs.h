#pragma once

// MeshCore's settings changed the way the badge changes them (#67): the badge's setters (mesh_task.cpp) and the host
// test (host_tests/companion_test.cpp) both go through these, so what the test saves and reloads is what the badge
// does. The caller holds the mesh lock and calls MyMesh::savePrefs() afterwards.
#include "NodePrefs.h"
#include "mesh_format.h"
#include "radio_presets.h"
#include "target.h"

#include <string.h>

// false (prefs untouched) when the name is empty after trimming; cut to 31 bytes on a character boundary
static inline bool mesh_prefs_set_name(NodePrefs *prefs, char const *name) {
    char clean[sizeof(prefs->node_name)];
    if (!mesh_name_clean(name, clean, sizeof(clean))) {
        return false;
    }
    memcpy(prefs->node_name, clean, sizeof(clean));
    return true;
}

// NULL when applied (the radio retuned, TX power set, path_hash_mode = size - 1), else the reason and nothing changed
static inline char const *mesh_prefs_set_radio(NodePrefs *prefs, float freq, float bw, uint8_t sf, uint8_t cr,
                                               uint8_t path_hash_size, int8_t tx_dbm) {
    char const *reason = radio_check(freq, bw, sf, cr, tx_dbm);
    if (reason) {
        return reason;
    }
    if (path_hash_size < 1 || path_hash_size > 3) { // MeshCore's CMD_SET_PATH_HASH_MODE takes modes 0..2
        return "Path hash size must be 1 to 3 bytes";
    }
    prefs->freq           = freq;
    prefs->bw             = bw;
    prefs->sf             = sf;
    prefs->cr             = cr;
    prefs->tx_power_dbm   = tx_dbm;
    prefs->path_hash_mode = (uint8_t)(path_hash_size - 1); // MyMesh floods with path_hash_mode + 1 bytes per hop
    radio_set_params(freq, bw, sf, cr);
    radio_set_tx_power((uint8_t)tx_dbm); // target.h takes it as uint8_t; LoraRadio reads it back as int8_t
    return NULL;
}
