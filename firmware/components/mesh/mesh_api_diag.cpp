// The air log (#165), Discover nearby (#167), the signal history (#166) and the health figures.
// Split out of mesh_task.cpp (#208); the state it uses is mesh_int.h's.
#include "mesh_int.h"

using namespace mesh_int;

static char const TAG[] = "mesh";

// ---- the air log (#165) ----------------------------------------------------------------------------------------
// The ring is written on the mesh task (inside MyMesh::loop(), mesh lock held) and read by the screen. Its own lock
// (a mutex: the ring and the screen's copy are in PSRAM, #271) covers only the push and the copy.

static airlog_t       airlog_ring;
static bool           airlog_failed;

extern "C" void mesh_airlog_note(uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, float rssi,
                                 float snr) {
    if (!airlog_ring.e) { // the first packet; only this task writes the pointer, readers see NULL or the whole ring
        if (airlog_failed) {
            return;
        }
        auto *mem = (airlog_entry_t *)lichen_alloc(meshcore_ctx(), (size_t)(AIRLOG_CAP) * sizeof(airlog_entry_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
        if (!mem) {
            airlog_failed = true;
            ESP_LOGW(TAG, "air log: no memory, packets are not listed");
            return;
        }
        section_enter<SECTION_AIRLOG>();
        airlog_init(&airlog_ring, mem, AIRLOG_CAP);
        section_exit<SECTION_AIRLOG>();
    }
    int16_t rssi_x10 = (int16_t)lroundf(rssi * 10.0f);
    int16_t snr_x10  = (int16_t)lroundf(snr * 10.0f);
    uint32_t at_s    = mesh_uptime_s();
    section_enter<SECTION_AIRLOG>();
    airlog_push(&airlog_ring, header, path_len, path, raw_len, rssi_x10, snr_x10, at_s);
    section_exit<SECTION_AIRLOG>();
}

extern "C" size_t mesh_airlog_snapshot(airlog_entry_t *out, size_t max, uint32_t *seq) {
    section_enter<SECTION_AIRLOG>();
    size_t n = airlog_ring.e ? airlog_copy(&airlog_ring, out, max) : 0;
    *seq     = airlog_ring.seq;
    section_exit<SECTION_AIRLOG>();
    return n;
}

// ---- Discover nearby (#167) ----------------------------------------------------------------------------------------
// The screen asks (any task, no mesh lock): the request is only marked pending; the mesh loop puts it on air under the
// lock and its listener feeds the answers into the table. The table has its own lock, as the air log: a mutex, as the
// answers' names come from the contacts and the screen's copy goes into its memory, both in PSRAM (#271).

static disc_t       disc_table; // zero = DISC_IDLE

extern "C" bool mesh_discover_start(char const **why) {
    char const *local;
    why  = why ? why : &local;
    *why = NULL;
    if (!send_ready(why)) {
        return false;
    }
    uint32_t tag;
    lichen_random_fill(&tag, sizeof(tag));
    section_enter<SECTION_DISCOVER>();
    bool ok = disc_start(&disc_table, tag, mesh_uptime_s(), why);
    section_exit<SECTION_DISCOVER>();
    return ok;
}

extern "C" void mesh_discover_get(disc_t *out) {
    section_enter<SECTION_DISCOVER>();
    disc_poll(&disc_table, mesh_uptime_s());
    *out = disc_table;
    section_exit<SECTION_DISCOVER>();
}

// From the mesh loop, under the lock: sends a pending request
void mesh_int::discover_tick_locked(void) {
    section_enter<SECTION_DISCOVER>();
    bool     pending = disc_table.state == DISC_PENDING;
    uint32_t tag     = disc_table.tag;
    section_exit<SECTION_DISCOVER>();
    if (!pending) {
        return;
    }
    char const *why = NULL;
    if (!ident_tx_allowed()) {
        why = "identity damaged: TX blocked";
    } else if (!lichen_radio_get(RADIO_LORA)) {
        why = "LoRa is switched off";
    } else {
        uint8_t req[DISC_REQ_LEN];
        disc_request(tag, req);
        mesh::Packet *p = node->createControlData(req, sizeof(req));
        if (p) {
            node->sendZeroHop(p);
        } else {
            why = "no free packet";
        }
    }
    section_enter<SECTION_DISCOVER>();
    if (why) {
        disc_failed(&disc_table, why);
    } else {
        disc_sent(&disc_table, mesh_uptime_s());
    }
    section_exit<SECTION_DISCOVER>();
    ESP_LOGI(TAG, "discover nearby: %s", why ? why : "request sent (zero hop)");
}

// An answer (a control packet) heard, under the lock; MyMesh passes every control packet here
void mesh_int::discover_note_locked(mesh::Packet const *pkt) {
    if (pkt->payload_len < 6 + DISC_PREFIX) {
        return;
    }
    char const *name = NULL;
    if (ContactInfo *c = node->lookupContactByPubKey(&pkt->payload[6], DISC_PREFIX)) {
        name = c->name;
    }
    int16_t snr_x10 = (int16_t)lroundf(pkt->getSNR() * 10.0f);
    section_enter<SECTION_DISCOVER>();
    disc_on_payload(&disc_table, pkt->payload, pkt->payload_len, snr_x10, name, mesh_uptime_s());
    section_exit<SECTION_DISCOVER>();
}

// ---- the signal history (#166) ---------------------------------------------------------------------------------------
// Written on the mesh task (mesh lock held) from the advert and message hooks, read by the contact's Signal page. As
// the air log: its own lock (a mutex, the table is in PSRAM, #271) covers only the note and the copy.

static sig_table_t  *signal_table;
static bool          signal_failed;

extern "C" void mesh_signal_note(uint8_t const *pub_key, bool is_advert, uint8_t path_len, uint8_t const *path,
                                 float rssi, float snr) {
    if (!signal_table) { // the first packet from a contact; only this task writes the pointer
        if (signal_failed) {
            return;
        }
        auto *mem = (sig_table_t *)lichen_alloc(meshcore_ctx(), sizeof(sig_table_t), LICHEN_MEM_BULK | LICHEN_MEM_ZERO);
        if (!mem) {
            signal_failed = true;
            ESP_LOGW(TAG, "signal history: no memory, contact signal graphs stay empty");
            return;
        }
        sig_init(mem);
        __atomic_store_n(&signal_table, mem, __ATOMIC_RELEASE);
    }
    int16_t  rssi_x10 = (int16_t)lroundf(rssi * 10.0f);
    int16_t  snr_x10  = (int16_t)lroundf(snr * 10.0f);
    uint32_t at_s     = mesh_uptime_s();
    section_enter<SECTION_SIGNAL>();
    sig_note(signal_table, pub_key, is_advert ? SIG_ADVERT : SIG_MESSAGE, rssi_x10, snr_x10, path_len, path, at_s);
    section_exit<SECTION_SIGNAL>();
}

extern "C" bool mesh_signal_get(uint8_t const prefix[6], sig_contact_t *out) {
    sig_table_t *t = __atomic_load_n(&signal_table, __ATOMIC_ACQUIRE);
    if (!t) {
        return false;
    }
    section_enter<SECTION_SIGNAL>();
    bool found = sig_get(t, prefix, out);
    section_exit<SECTION_SIGNAL>();
    return found;
}

#define HEALTH_NOISE_MAX_AGE_S 60 // the C6's channel reading older than this is not shown

// Lock order: only the mesh lock, nothing else held; lora_get_channel_rssi takes the LoRa state spinlock for a copy
extern "C" bool mesh_health_snapshot(mesh_health_t *out, uint32_t wait_ms) {
    mesh_health_t h = {};
    if (!__atomic_load_n(&node, __ATOMIC_ACQUIRE) || mesh_is_stopped() || !mesh_lock_within(wait_ms)) {
        return false;
    }
    node->fillHealth(&h);
    h.rx_dropped  = lora_radio.rx_dropped();
    h.send_failed = lora_radio.send_failed;
    mesh_unlock();
    h.have_noise = lora_get_channel_rssi(&h.noise_x10, HEALTH_NOISE_MAX_AGE_S);
    *out         = h;
    return true;
}
