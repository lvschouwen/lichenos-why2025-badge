#pragma once

#include "mesh_contact_fmt.h"
#include "mesh_outbox.h"
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
    uint8_t  pubkey[6]; // the start of the public key: shown, and the DM prefix (#47)
    char     name[MESH_NAME_MAX + 1];
    uint8_t  type;     // MeshCore ADV_TYPE_*
    uint8_t  flags;    // MESH_CONTACT_FAV: a favourite (#49)
    uint8_t  path_len; // MeshCore's out_path_len: hops and hash size packed (mesh_path_decode), 0xFF = flood
    uint32_t age_s;    // since its last advert was heard
} mesh_contact_t;

// MeshCore as a Lichen app (meshcore_app.c, #67): its tile, screens, Settings page and background part. Register it
// before shell_start(), so it comes first on home; main.c then runs its start_background with the other apps'.
void meshcore_app_register(void);

// Starts MeshCore with its directory (#84 T6): false when it did not start. The first start loads everything (identity,
// contacts, the history journal); after a mesh_stop(MESH_STOP_USER) it only claims the radio again and restarts the
// task (#103). True at once when it runs; false after a stop for an update. Blocks for up to a few seconds: boot or a
// job task, never the LVGL task. Starts and stops are serialised.
bool mesh_start(char const *files_dir);
// The task runs (started, not stopped)
bool mesh_running(void);
typedef enum {
    MESH_STOP_USER,   // on request (#103): mesh_start brings it back
    MESH_STOP_UPDATE, // a firmware update is coming (#76): final, no start before the restart
} mesh_stop_reason_t;
// Saves prefs, contacts and channels, flushes the history journal, then the mesh task ends and the LoRa claim is
// released. Waits for that (up to 10 s): true when it is stopped (or was not running), false when it did not stop in
// time (it still stops, and releases the radio, when the task gets there). The update stop also settles the journal's
// held messages and wipes its key; it works on a MeshCore already stopped on request too.
bool mesh_stop(mesh_stop_reason_t why);
// A stop was asked and the task has not ended yet (a stop that outlasted mesh_stop's wait): no start meanwhile
bool mesh_stop_pending(void);
// "fw mesh", "fw contacts" and friends: registered once, whether MeshCore starts or not (#103)
void mesh_console_init(void);
// The node id (the start of the public key) as 8 hex digits, taken once at mesh_start: no lock, so the screens may
// call it any time. false before MeshCore is up.
bool mesh_node_id(char *hex, size_t cap);

// The home tile (#67): a copy of what the mesh task keeps up to date (a spinlock, never the mesh lock), safe any time
void mesh_tile_snapshot(mesh_tile_t *out);

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
// A zero-hop advert now: true when queued (the home tile snapshot then has advert_sent and its age)
bool mesh_send_advert(void);
// The channel names in slot order, up to max; MESH_BUSY when the lock wasn't free within MESH_SET_WAIT_MS
size_t mesh_channels_snapshot(char names[][MESH_CHANNEL_MAX + 1], size_t max);
// The same, waiting at most wait_ms for the lock (screens call it on the LVGL task)
size_t mesh_channels_snapshot_wait(char names[][MESH_CHANNEL_MAX + 1], size_t max, uint32_t wait_ms);

// The companion's FIRMWARE_VERSION ("v1.18.0")
char const *mesh_firmware_version(void);

// The one clock for "N min ago": seconds since boot (64-bit source, doesn't wrap)
uint32_t mesh_uptime_s(void);
// The wall clock for message ages (#48): Unix seconds, 0 while the clock is not set
uint32_t mesh_epoch_now(void);

// The snapshots wait at most wait_ms for the mesh lock (MeshCore holds it through its loop, including file saves
// that can take a while): MESH_BUSY when it wasn't free in time, so the screen skips a tick instead of freezing.
#define MESH_BUSY         SIZE_MAX
#define MESH_WAIT_FOREVER UINT32_MAX
// Up to max of the newest messages over all chats, oldest first; returns how many, *generation as mesh_store_view_generation()
size_t mesh_messages_snapshot(mesh_msg_t *out, size_t max, uint32_t *generation, uint32_t wait_ms);
// One chat's messages (the newest max, oldest first), *generation as mesh_store_view_generation(). With mark_read the
// chat's unread count is cleared in the same lock and the home tile follows: call it while the chat is on screen.
size_t mesh_chat_snapshot(mesh_chat_key_t const *key, mesh_msg_t *out, size_t max, uint32_t *generation,
                          bool mark_read, uint32_t wait_ms);
// The chats with unread counts and the latest line, in table order (the caller sorts: mesh_conv_sort). out holds up to
// max chats (MESH_CONV_MAX is about 15 KB): a PSRAM buffer, never the stack.
size_t mesh_convs_snapshot(mesh_conv_t *out, size_t max, uint32_t wait_ms);
// Up to max contacts, favourites first, then the most recently heard; returns how many, *total = all contacts
size_t mesh_contacts_snapshot(mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms);
// The same for one of the grid's lists (#88, mesh_contact_view_t): only the view's contacts, in its order; *total =
// all contacts in the view (max 0 just counts them)
size_t mesh_contacts_view(mesh_contact_view_t view, mesh_contact_t *out, size_t max, size_t *total, uint32_t wait_ms);
// The grid's badges (#88) in one pass under the lock, without copying contacts: false (out unchanged) when the lock was
// not free within wait_ms; zeros when MeshCore is not running
typedef struct {
    size_t   total, repeaters; // all contacts; those in MESH_VIEW_REPEATERS
    bool     heard_any;
    uint32_t newest_age_s;     // since the most recently heard contact
} mesh_contact_stats_t;
bool mesh_contacts_stats(mesh_contact_stats_t *out, uint32_t wait_ms);
// One contact by its 6-byte key prefix (#49): 1, 0 when it is not (or no longer) there, MESH_BUSY when the lock was
// not free in time or MeshCore is not running (not up yet, stopped for an update)
size_t mesh_contact_detail(uint8_t const prefix[6], mesh_contact_detail_t *out, uint32_t wait_ms);
// Reset path / favourite on or off / remove (#49): handed to MyMesh as its own command, which it runs in its next loop
// and saves with its contacts a few seconds later (Remove also drops the contact's stored advert). Reset path and the
// favourite flag show in a copy taken right after. 1 when queued, 0 when the contact is gone, MESH_BUSY when the lock
// was not free within wait_ms, MyMesh's command queue is full or MeshCore is not running.
size_t mesh_contact_do(uint8_t const prefix[6], mesh_contact_op_t op, uint32_t wait_ms);

// ---- sending (#45; the compose screens are #46 and #47) --------------------------------------------------------------
// All copy-in and non-blocking: they never take the mesh lock (LVGL safe). The mesh task sends from the outbox
// (mesh_outbox.h) through MyMesh's own command frames and tracks the state of each message.

// Queues a channel message: the channel is named (slots can change), the text is cut at mesh_channel_text_max() on a
// UTF-8 boundary. 0 with *why set ("busy", "empty", "MeshCore is not running"...). Sent as "<node name>: <text>".
mesh_msg_id_t mesh_send_channel(char const *channel_name, char const *text, char const **why);
// A DM to the contact with this 6-byte public key prefix; retried up to 3 times (reset path, then flood), see the outbox
mesh_msg_id_t mesh_send_dm(uint8_t const prefix[6], char const *text, char const **why);
// A failed message starts over under the same id
bool mesh_retry(mesh_msg_id_t id);
// false when the id is unknown (evicted: the table keeps 16). Any pointer may be NULL.
bool mesh_out_state(mesh_msg_id_t id, mesh_out_state_t *st, uint8_t *attempt, uint8_t *repeats);
// Bytes of text that fit in a channel message: 160 minus "<node name>: "
size_t mesh_channel_text_max(void);

typedef struct {
    mesh_msg_id_t    id;
    bool             is_dm;
    mesh_out_state_t state;
    uint8_t          attempt, repeats;
    uint32_t         trip_ms;
    char             target[OB_TARGET_MAX]; // channel name, or the contact prefix in hex
} mesh_out_info_t;
// The newest messages first, up to max (for the console and the chat screens)
size_t mesh_out_list(mesh_out_info_t *out, size_t max);

#ifdef __cplusplus
}
#endif
