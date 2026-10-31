#pragma once

#include "mesh_store.h"
#include "mesh_tile.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// MeshCore's companion (examples/companion_radio MyMesh) on the badge (#61, #65): contacts, channels and identity
// persisted in /storage/meshcore, the screens fed through the functions below. Every
// function here takes the mesh lock itself, and copies: callers never hold it.

#define MESH_NAME_MAX 32

typedef struct {
    uint8_t  pubkey[4]; // the start of the public key, for display
    char     name[MESH_NAME_MAX + 1];
    uint8_t  type;     // MeshCore ADV_TYPE_*
    uint8_t  path_len; // hops of the known path, 0xFF when only flood routing works
    uint32_t age_s;    // since its last advert was heard
} mesh_contact_t;

// MeshCore as a Lichen app (meshcore_app.c, #67): its tile, screens, Settings page and background part. Register it
// before shell_start(), so it comes first on home; main.c then runs its start_background with the other apps'.
void meshcore_app_register(void);

bool mesh_start(void); // the app's start_background: false when it did not start
// The task runs (started, not stopped for an update)
bool mesh_running(void);
// A firmware update is coming (#76): saves prefs, contacts and channels, then the mesh task ends. Waits for that (up to
// 10 s); true when it is stopped (or was not running), false when it did not stop in time. MeshCore does not start again before the restart. The console commands then say "MeshCore stopped for update".
bool mesh_stop_for_update(void);
// The node id (the start of the public key) as 8 hex digits, taken once at mesh_start: no lock, so the screens may
// call it any time. false before MeshCore is up.
bool mesh_node_id(char *hex, size_t cap);

// The home tile (#67): a copy of what the mesh task keeps up to date (a spinlock, never the mesh lock), safe any time
void mesh_tile_snapshot(mesh_tile_t *out);
// Messages is on screen: what arrived so far counts as read
void mesh_mark_seen(void);

// The radio as MeshCore's prefs have it; the prefs are read under the mesh lock (up to wait_ms), else the copy taken at
// start is given. false before MeshCore is up.
typedef struct {
    uint32_t freq_hz, bw_hz;
    uint8_t  sf, cr;
    int8_t   tx_dbm;
} mesh_radio_t;
bool mesh_radio_params(mesh_radio_t *out, uint32_t wait_ms);
// MeshCore's settings (#67). The setters take the mesh lock (waiting at most MESH_SET_WAIT_MS: MeshCore may be saving
// contacts), change MyMesh's prefs as its own app commands do, and save them. They can block for a few hundred ms, so
// the screens call them off the LVGL task. mesh_get_settings never waits: a copy kept by the setters.
#define MESH_SET_WAIT_MS 3000
typedef struct {
    char    name[MESH_NAME_MAX + 1];
    float   freq, bw; // MHz, kHz
    uint8_t sf, cr, path_hash_size;
    int8_t  tx_dbm;
} mesh_settings_t;
// false before MeshCore is up (out zeroed)
bool mesh_get_settings(mesh_settings_t *out);
// Trimmed, cut to 31 bytes; false when empty, MeshCore isn't running or is busy. Others see it in the next advert.
bool mesh_set_name(char const *name);
// NULL when applied (radio retuned and saved), else the reason (radio_check()'s, or busy / not running): nothing changed
char const *mesh_set_radio(float freq, float bw, uint8_t sf, uint8_t cr, uint8_t path_hash_size, int8_t tx_dbm);
// A channel on the first free slot, saved. NULL key: a #hashtag channel (key from its lowercased name, as MeshCore's
// apps do); else a base64 key of 16 or 32 bytes. NULL when added, else the reason.
char const *mesh_channel_add(char const *name, char const *b64_key_or_null);
// Empties a channel's slot and saves (the other channels keep their slot numbers). NULL when removed, else the reason:
// slot 0 (Public) and empty slots are refused. _named picks it by name, without case. Same lock rules as the setters.
char const *mesh_channel_remove(int index);
char const *mesh_channel_remove_named(char const *name);
// A zero-hop advert now: true when queued
bool mesh_send_advert(void);
// The channel names in slot order, up to max; MESH_BUSY when the lock wasn't free within MESH_SET_WAIT_MS
size_t mesh_channels_snapshot(char names[][MESH_CHANNEL_MAX + 1], size_t max);

// The companion's FIRMWARE_VERSION ("v1.18.0")
char const *mesh_firmware_version(void);

// The one clock for "N min ago": seconds since boot (64-bit source, doesn't wrap)
uint32_t mesh_uptime_s(void);

// The snapshots wait at most wait_ms for the mesh lock (MeshCore holds it through its loop, including file saves
// that can take a while): MESH_BUSY when it wasn't free in time, so the screen skips a tick instead of freezing.
#define MESH_BUSY         SIZE_MAX
#define MESH_WAIT_FOREVER UINT32_MAX
// Up to max of the newest messages, oldest first; returns how many, *generation as mesh_store_generation()
size_t mesh_messages_snapshot(mesh_msg_t *out, size_t max, uint32_t *generation, uint32_t wait_ms);
// Up to max contacts, most recently heard first; returns how many, *total = all contacts
size_t mesh_contacts_snapshot(mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif
