#pragma once
// The badge's end of MeshCore's companion protocol (#45, #55): MyMesh talks to a "serial interface" and the badge has
// no phone, so this in-RAM pipe is that interface. It is connected (so MyMesh produces send confirmations, login and
// status replies, path updates), drains MyMesh's offline queue (the screens get messages from the Listener instead),
// and turns the outbox (mesh_outbox.h) into CMD frames. Everything here runs on the mesh task inside MyMesh::loop(),
// under the mesh lock; only the outbox is shared, through mesh_ob_enter/exit (a mutex, as the outbox is in PSRAM, #271;
// defined by the platform: mesh_task.cpp on the badge, a no-op in the host test).
#include "advert_limit.h"
#include "mesh_blobq.h"
#include "mesh_contact_fmt.h"
#include "mesh_frame.h"
#include "mesh_health.h"
#include "mesh_outbox.h"
#include "mesh_remote.h"
#include "mesh_trace.h"
#include "MyMesh.h"

extern "C" {
void mesh_ob_enter(void);
void mesh_ob_exit(void);
// Adverts from new nodes are being dropped (#6): told once per spell, with the total since boot (mesh_task.cpp logs it)
void mesh_advert_limited(uint32_t refused);
// The contact table is full and an advert of a new node was not added (#52): called for each such advert, under the
// mesh lock (mesh_task.cpp raises a coalesced notice)
void mesh_contacts_full(void);
// A packet MeshCore received (#165), headers only, on the mesh task under the mesh lock: header and path_len as the
// packet carries them, the path bytes, bytes on air, RSSI in dBm and SNR in dB. mesh_task.cpp keeps the air log ring.
void mesh_airlog_note(uint8_t header, uint8_t path_len, uint8_t const *path, uint8_t raw_len, float rssi, float snr);
// An advert or a direct message from a contact in the table (#166), on the mesh task under the mesh lock: its public
// key (the first 6 bytes name it), the signal of the packet and, for an advert, the path it came by. mesh_task.cpp
// keeps it.
void mesh_signal_note(
    uint8_t const *pub_key, bool is_advert, uint8_t path_len, uint8_t const *path, float rssi, float snr
);
}

#define LINK_PUSH_KINDS 0x11 // push codes 0x80..0x90
#define LINK_FIFO       8
#define LINK_OVF        4 // the badge's own commands (#49) that found the FIFO full wait here, not dropped (#112)

struct LinkStats {
    uint32_t cmds_out = 0, replies = 0, pushes = 0, msg_waiting = 0, drained = 0, no_more = 0;
    uint32_t self_ok = 0, self_bad = 0, lost_replies = 0, fifo_drops = 0, bad_frames = 0, confirmed = 0;
    uint32_t traces = 0, discoveries = 0, paths_stored = 0; // #53: our answers in, discovered paths stored
    uint32_t remote_answers = 0, cli_replies = 0;           // #51: login/status answers and CLI replies that were ours
    uint32_t push_count[LINK_PUSH_KINDS] = {};
    uint32_t reply_count[32]             = {};
    uint8_t  app_ver                     = 0; // what DEVICE_INFO answered with (the firmware's protocol version)
};

class BadgeMesh;

class CompanionPipe : public BaseSerialInterface {
  public:
    void attach(MyMesh *mesh, ob_t *ob) {
        mesh_ = mesh;
        ob_   = ob;
    }
    // The trace/discovery request shared with the UI (#53), under mesh_ob_enter/exit like the outbox
    void attach_trace(mt_t *mt) {
        mt_ = mt;
    }
    mt_t *trace() const {
        return mt_;
    }
    // The log in / status / CLI request shared with the UI (#51), under mesh_ob_enter/exit as well
    void attach_remote(rr_t *rr) {
        rr_ = rr;
    }
    rr_t *remote() const {
        return rr_;
    }
    // Queues the start sequence: DEVICE_QUERY(3) so MyMesh builds the V3 message layouts, APP_START, then a drain
    void             start();
    LinkStats const &stats() const {
        return stats_;
    }
    bool draining() const {
        return draining_;
    }

    void enable() override {
    }
    void disable() override {
    }
    bool isEnabled() const override {
        return true;
    }
    bool isConnected() const override {
        return true;
    }
    bool isWriteBusy() const override {
        return false;
    }
    size_t writeFrame(uint8_t const src[], size_t len) override;
    size_t checkRecvFrame(uint8_t dest[]) override;
    // A command of the badge's own (#49) for MyMesh's next loop, like the start sequence; false when the queue is full
    bool   queue_cmd(uint8_t const *frame, size_t len);
    bool   can_queue() const {
        return (fifo_n_ < LINK_FIFO && ovf_n_ == 0) || ovf_n_ < LINK_OVF;
    }

  private:
    enum CurKind : uint8_t { CUR_NONE, CUR_INTERNAL, CUR_OUTBOX, CUR_TRACE, CUR_REMOTE };
    struct Frame {
        uint8_t len;
        uint8_t data[MF_MAX_FRAME];
    };
    bool   fifo_push(uint8_t const *data, size_t len);
    size_t take_fifo(uint8_t dest[]);
    size_t take_outbox(uint8_t dest[]);
    size_t take_trace(uint8_t dest[]);
    size_t take_remote(uint8_t dest[]);
    void   store_discovered_path(mf_discovery_t const &d);
    void   request_drain();
    size_t build_outbox_frame(ob_action_t const &act, uint8_t *dest);
    void   on_reply(mf_msg_t const &m);
    void   on_push(mf_msg_t const &m);

    MyMesh       *mesh_ = nullptr;
    ob_t         *ob_   = nullptr;
    mt_t         *mt_   = nullptr;
    rr_t         *rr_   = nullptr;
    mt_plan_t     plan_; // off the stack, as scratch_
    Frame         fifo_[LINK_FIFO];
    uint8_t       fifo_head_ = 0, fifo_n_ = 0;
    Frame         ovf_[LINK_OVF];
    uint8_t       ovf_head_ = 0, ovf_n_ = 0;
    void          refill_from_ovf();
    CurKind       cur_        = CUR_NONE;
    uint8_t       cur_cmd_    = 0;
    mesh_msg_id_t cur_id_     = 0;
    bool          draining_   = false;
    uint8_t      *wipe_frame_ = nullptr; // the last login/CLI frame handed out, wiped at the next call
    size_t        wipe_len_   = 0;
    ob_arb_t      arb_        = {};
    LinkStats     stats_;
    mf_msg_t      scratch_; // off the mesh task's stack: writeFrame runs deep inside loop()
    ob_action_t   act_;
};

// MyMesh plus the badge's hooks: the pipe as its interface, and the radio's view of our own packets
class BadgeMesh : public MyMesh {
  public:
    BadgeMesh(
        mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore &store, ob_t *ob
    )
        : MyMesh(radio, rng, rtc, tables, store), ob_(ob), store_(store) {
        pipe_.attach(this, ob);
        advert_limit_init(&advert_limit_);
        mesh_blobq_init(&blobs_);
        blob_taken_.len = 0;
    }
    // MyMesh's own filter for the contacts file: not the transient entries
    static bool keepContact(ContactInfo const &c) {
        return c.type != ADV_TYPE_NONE;
    }
    // Advert blobs written after the mesh lock (#257, mesh_blobq.h). takeBlob at the end of the mesh task's locked part
    // (no packet in this loop: quiet) picks one and first takes the one written last time off the queue;
    // writeTakenBlob then writes it without the lock, on the mesh task (a no-op when none was picked). The queued copy
    // stays readable meanwhile; the queue is only touched under the mesh lock, and the copy being written is the mesh
    // task's own.
    void   takeBlob(bool quiet);
    void   writeTakenBlob();
    // Under the lock, before a stop or a restart: everything queued goes to flash now
    void   flushBlobs();
    // MeshCore's files were erased (factory reset, "erase", "rebuild"): a queued blob must not bring one back
    void   dropBlobs();
    size_t blobsQueued() const {
        return mesh_blobq_count(&blobs_);
    }
    // Prefs, contacts, channels and the queued blobs, under the lock: MeshCore's own reboot
    void saveAll();
    // MyMesh's loop (hides it: not virtual, the mesh task calls it through BadgeMesh). MyMesh deletes a blob file only
    // in its loop (a removed or overwritten contact; DataStore::deleteBlobByKey is not virtual), so a blob taken but
    // not yet written may belong to a contact that is gone after it: it is not written then, and takeBlob checks again.
    void loop() {
        if (!blob_written_) {
            blob_taken_.len = 0;
        }
        MyMesh::loop();
    }
    CompanionPipe &pipe() {
        return pipe_;
    }
    void attachTrace(mt_t *mt) {
        pipe_.attach_trace(mt);
    }
    void attachRemote(rr_t *rr) {
        pipe_.attach_remote(rr);
    }
    // Log out of a repeater or room (#51), under the mesh lock: MyMesh stops its keep-alives to it. Nothing goes on
    // air. False when the contact is gone or the command queue is full.
    bool remoteLogout(uint8_t const prefix[MF_PREFIX]);
    // Our own advert, under the mesh lock (#52): zero-hop (heard by nodes in direct range) or flooded with the default
    // scope as MyMesh's CMD_SEND_SELF_ADVERT does. With the location when the prefs share it. false when no packet.
    bool advertNow(bool flood);
    // A contact change from the details screen (#49), under the mesh lock. It goes to MyMesh as its own command
    // (RESET_PATH, ADD_UPDATE_CONTACT, REMOVE_CONTACT), so MyMesh saves the contacts itself (its lazy 5 s write) and
    // Remove also drops the stored advert. Reset path and the favourite flag are set at once too, so a copy taken right
    // after shows them.
    enum ContactResult { CONTACT_QUEUED, CONTACT_GONE, CONTACT_QUEUE_FULL };
    ContactResult contactAction(uint8_t const prefix[MF_PREFIX], mesh_contact_op_t op);
    // Resets every stored route whose hash size is not hash_size (#130); the count. The caller saves the contacts.
    int           forgetStaleRoutes(uint8_t hash_size);
    ContactResult contactSetPath(uint8_t const prefix[MF_PREFIX], uint8_t path_len, uint8_t const *path);
    // MeshCore stops on request (#103), under the mesh lock: the packets queued for the radio are dropped, so none
    // goes out late after the next start. Returns how many.
    int           dropOutbound();
    // MeshCore's own counters for the radio health page (#157), under the mesh lock: what upstream's CMD_GET_STATS
    // answers (core, radio, packets). Reads only. The C6 reading and the LoRa task's drop counts are filled by the
    // caller.
    void          fillHealth(mesh_health_t *h);
    void          startPipe() {
        startInterface(pipe_);
        pipe_.start();
    }

  protected:
    void onContactsFull() override {
        MyMesh::onContactsFull();
        mesh_contacts_full();
    }
    void logRxRaw(float snr, float rssi, uint8_t const raw[], int len) override {
    } // no LOG_RX_DATA push per packet
    void logRx(mesh::Packet *packet, int len, float score) override;
    void logTx(mesh::Packet *packet, int len) override;
    void logTxFail(mesh::Packet *packet, int len) override;
    // A room's post (#51): the screens get "<author>: <text>", as the room's own chat shows it
    void onSignedMessageRecv(
        ContactInfo const &from,
        mesh::Packet      *packet,
        uint32_t           sender_timestamp,
        uint8_t const     *sender_prefix,
        char const        *text
    ) override;
    // Verified adverts: a node not in the contact table goes through the new-node rate limit first (#6)
    void onAdvertRecv(
        mesh::Packet *packet, mesh::Identity const &id, uint32_t timestamp, uint8_t const *app_data, size_t app_data_len
    ) override;
    // A direct message from a contact: its signal goes to the signal history (#166), then MyMesh queues the message
    void onMessageRecv(
        ContactInfo const &from, mesh::Packet *packet, uint32_t sender_timestamp, char const *text
    ) override;
    // The raw advert of a contact (BaseChatMesh, for "share"): queued instead of written inside the loop (#257)
    bool putBlobByKey(uint8_t const key[], int key_len, uint8_t const src_buf[], int len) override;
    int  getBlobByKey(uint8_t const key[], int key_len, uint8_t dest_buf[]) override;

  private:
    bool               blobWanted(mesh_blobq_entry_t const &e);
    void               blobDone();
    ob_t              *ob_;
    DataStore         &store_;
    mesh_blobq_t       blobs_;
    mesh_blobq_entry_t blob_taken_;           // the one writeTakenBlob writes; len 0: none
    bool               blob_written_ = false; // blob_taken_ is on flash: off the queue at the next takeBlob
    CompanionPipe      pipe_;
    advert_limit_t     advert_limit_;
};
