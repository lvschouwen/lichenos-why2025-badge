#pragma once
// The message history journal on the SD card (#48): the file side of mesh_hist.c, for mesh_task.cpp only.
// /sd/lichen/messages/log.bin is appended to; log.tmp exists only while a compaction writes it. The records are
// encrypted with a key derived from the node's identity (#98, see mesh_hist.h).
//
// Threading: the _locked functions run on the mesh task with the mesh lock held and only touch memory. The file work
// (mesh_journal_flush) runs on the mesh task after mesh_unlock(), inside one sd_begin/sd_end batch, so the screens
// never wait on the card. The one exception is the stop for an update, see mesh_journal_stop_locked.

#include "mesh_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Boot, after MeshCore's begin() loaded the identity (prv: its private key, for the journal key, #98; not kept;
// NULL when it could not be read: history off) and
// before the mesh task runs: replays the journal into the store (taking lock/unlock around each store change, the
// screens may already be reading it), asks the mesh task for a compaction when needed, and checks the card. Persistence is off (one
// ESP_LOGW, RAM history keeps working) without a card, on a full card, or when memory or the journal fail.
void mesh_journal_boot(size_t keep, void (*lock)(void), void (*unlock)(void), uint8_t const *prv, size_t prv_len);

// A message just added to the store (the chat's newest): incoming ones are journaled at once, outgoing ones are held
// until their state is final
void mesh_journal_incoming_locked(mesh_chat_key_t const *key);
void mesh_journal_outgoing_locked(mesh_chat_key_t const *key);
void mesh_journal_out_state_locked(uint32_t id, uint8_t state, uint8_t repeats, uint8_t attempt, uint32_t trip_ms);
// The outbox holds only these ids now
void mesh_journal_out_sweep_locked(uint32_t const *ids, size_t n);

// Once per mesh loop, under the lock: when a flush is due, the pending buffer is swapped out for mesh_journal_flush
void mesh_journal_tick_locked(void);
// Once per mesh loop, after mesh_unlock(): writes a swapped-out buffer (and compacts when asked or over the cap)
void mesh_journal_flush(void);
// The update stop, with the lock held (MeshCore saves its own files under it too): held outgoing messages are written
// as they stand ("not sent" when still pending) and everything pending goes to the card before MeshCore is gone
void mesh_journal_stop_locked(void);

// "fw mesh hist [compact]"
void mesh_journal_console(char const *args);

#ifdef __cplusplus
}
#endif
