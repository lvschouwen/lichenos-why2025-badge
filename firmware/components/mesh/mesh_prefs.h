#pragma once

// MeshCore's settings changed the way the badge changes them (#67): the badge's setters (mesh_task.cpp) and the host
// test (host_tests/companion_test.cpp) both go through these, so what the test saves and reloads is what the badge
// does. The caller holds the mesh lock and calls MyMesh::savePrefs() afterwards.
#include "NodePrefs.h"
#include "mesh_advert_cfg.h"
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

// ---- auto-add, location (#52) ----------------------------------------------------------------------------------------

static inline void mesh_prefs_get_autoadd(NodePrefs const *prefs, advcfg_autoadd_t *out) {
    advcfg_autoadd_decode(prefs->manual_add_contacts, prefs->autoadd_config, prefs->autoadd_max_hops, out);
}

// MyMesh reads the three prefs on every advert, so the change is live at once
static inline void mesh_prefs_set_autoadd(NodePrefs *prefs, advcfg_autoadd_t const *in) {
    advcfg_autoadd_encode(in, &prefs->manual_add_contacts, &prefs->autoadd_config, &prefs->autoadd_max_hops);
}

// NULL when set, else the reason (nothing changed). Like CMD_SET_ADVERT_LATLON the position lives in `sensors`, which
// MyMesh::savePrefs copies into the prefs; the prefs get it here as well.
static inline char const *mesh_prefs_set_location(NodePrefs *prefs, double lat, double lon) {
    if (!(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0)) { // false for NaN as well
        return "latitude is -90 to 90, longitude -180 to 180";
    }
    sensors.node_lat = lat;
    sensors.node_lon = lon;
    prefs->node_lat  = lat;
    prefs->node_lon  = lon;
    if (!advcfg_location_set(lat, lon)) {
        prefs->advert_loc_policy = ADVERT_LOC_NONE; // no place to share any more
    }
    return NULL;
}

// "Share location in advert": off by default. On needs a location (0, 0 counts as none).
static inline char const *mesh_prefs_set_loc_share(NodePrefs *prefs, bool on) {
    if (on && !advcfg_location_set(sensors.node_lat, sensors.node_lon)) {
        return "set a location first";
    }
    prefs->advert_loc_policy = on ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE;
    return NULL;
}
