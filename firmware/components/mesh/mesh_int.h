#pragma once
// The mesh task's own state, shared by the files split out of mesh_task.cpp (#208): mesh_task.cpp keeps the task, its
// start and stop and the state below (defined there); mesh_api_*.cpp hold the C API of mesh.h and friends, mesh_cmd*.cpp
// the console. C++ only and private to the mesh component (in namespace mesh_int, so nothing here is a global symbol).
// Every piece keeps the task and the lock that own it, as documented at its definition.
#include "mesh_wipe.h"
#include "mesh_rset.h"
#include "mesh_telem.h"
#include "mesh_link.h"

#include "lora_radio.h"
#include "mesh.h"
#include "mesh_format.h"
#include "mesh_advert_cfg.h"
#include "mesh_chat.h"
#include "mesh_cscope.h"
#include "mesh_contact_fmt.h"
#include "mesh_frame.h"
#include "mesh_hist_crypto.h"
#include "mesh_ident.h"
#include "mesh_journal.h"
#include "meshcore_run.h"
#include "mesh_rset_run.h"
#include "mesh_rhist_run.h"
#include "channel_input.h"
#include "mesh_pathhash.h"
#include "mesh_mem.h"
#include "mesh_prefs.h"
#include "mesh_share.h"
#include "radio_presets.h"
#include "mesh_radio_line.h"

#include "SPIFFS.h"

#include <SHA256.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/TxtDataHelpers.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "lichen_badge.h"
#include "lichen_fs.h"
#include "lichen_notify.h"
#include "lichen_radio.h"
#include "lichen_settings.h"
#include "mesh_notify_run.h"
#include "lichen_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lichen_mem.h"
#include "lichen_task.h"

#include "fw_console.h"
#include "lora_app.h"

#include <strings.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STORE_MESSAGES 100 // kept per chat in mesh_store (PSRAM, allocated with the chat's first message)

namespace mesh_int {

// ---- mesh_task.cpp ------------------------------------------------------------------------------------------------

extern LoraRadio      lora_radio;
extern DataStore      store;
extern BadgeMesh     *node;
extern ob_t          *outbox;            // PSRAM; shared with the UI under the outbox lock (mesh_link.h)
extern volatile int   offline_queue_len; // MyMesh's offline queue, from the Listener (#55: stays ~0 now)
extern TaskHandle_t   mesh_task_handle;  // valid only while mesh_running()
extern mesh_ident_t   ident_state;       // the stored identity's check at load (#80, ident_verify)
extern uint32_t       tile_advert_s;     // uptime + 1 of our last advert, 0 for none
extern portMUX_TYPE   tile_mux;          // the home tile's numbers and the advert schedule (adv)
extern bool (*const keep_contact)(ContactInfo const &c); // what a save keeps (MeshCore writes contacts lazily)

// The locks of data that lives in PSRAM (the outbox, trace and remote requests, air log, signal history) or is copied
// from or into it (the discovery table): FreeRTOS mutexes, not spinlocks, as a critical section must never touch PSRAM,
// whose cache can be off during a flash write (lichen_task.c, #271). Never take one from an ISR or inside a critical
// section, and never block (no other lock, no wait, no I/O) while holding one: the sections stay short copies. Made on
// first use (thread-safe statics), so a screen may ask before MeshCore starts.
enum section_t { SECTION_OUTBOX, SECTION_AIRLOG, SECTION_DISCOVER, SECTION_SIGNAL };
template <section_t S> SemaphoreHandle_t section_mutex(void) {
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t handle = xSemaphoreCreateMutexStatic(&buf);
    return handle;
}
template <section_t S> void section_enter(void) {
    xSemaphoreTake(section_mutex<S>(), portMAX_DELAY);
}
template <section_t S> void section_exit(void) {
    xSemaphoreGive(section_mutex<S>());
}

void        mesh_lock(void);
bool        mesh_lock_within(uint32_t wait_ms);
void        mesh_unlock(void);
bool        mesh_is_stopped(void);
char const *stopped_why(void); // why a stopped MeshCore refuses, for the setters and senders
bool        ident_tx_allowed(void);
void        ident_apply(mesh_ident_t state, bool simulated);
void        tile_publish_unread(void); // under the mesh lock
uint32_t    tile_unread(void);

// The periodic advert (#52), under tile_mux: the settings' copy, when the last went out and when the next is due
// (Unix seconds, 0 for none). Stored in the app's settings (adv_load): the schedule survives restarts, so a badge that
// is on for an hour a day still advertises once per interval and a reboot loop never floods.
struct adv_state_t {
    uint32_t interval_min = ADVCFG_INTERVAL_DEFAULT;
    bool     flood_manual = false; // the manual Advertise: zero-hop unless set
    uint32_t last_epoch   = 0;
    uint32_t next_epoch   = 0;
    uint32_t last_uptime  = 0; // uptime + 1 of the last advert of ours, for the minimum gap
};
extern adv_state_t adv;

// ---- mesh_api_settings.cpp ----------------------------------------------------------------------------------------

void        settings_from_node(MyMesh *n); // under the mesh lock
char const *settings_lock(void);           // NULL with the mesh lock taken, else why not
void        adv_load(void);                // at start, before the task runs
void        advert_tick_locked(void);      // the mesh task, under the mesh lock
void        contacts_full_post(void);      // the mesh task, after the unlock

// ---- mesh_api_channels.cpp ----------------------------------------------------------------------------------------

int channel_find_locked(char const *typed); // under the mesh lock

// ---- mesh_api_send.cpp --------------------------------------------------------------------------------------------

bool send_ready(char const **why, mesh_origin_t from = MESH_FROM_SCREEN);
extern mt_t     *trace_req;  // PSRAM, made with the outbox
extern rr_t     *remote_req; // PSRAM, made with the outbox
extern tl_rate_t telem_rate; // under mesh_ob_enter/exit

// ---- mesh_api_diag.cpp --------------------------------------------------------------------------------------------

void discover_note_locked(mesh::Packet const *pkt); // #167, from MyMesh's loop
void discover_tick_locked(void);                    // the mesh task, under the mesh lock

} // namespace mesh_int
