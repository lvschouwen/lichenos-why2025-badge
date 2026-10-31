#pragma once
// The badge's end of MeshCore's companion protocol (#45, #55): MyMesh talks to a "serial interface" and the badge has
// no phone, so this in-RAM pipe is that interface. It is connected (so MyMesh produces send confirmations, login and
// status replies, path updates), drains MyMesh's offline queue (the screens get messages from the Listener instead),
// and turns the outbox (mesh_outbox.h) into CMD frames. Everything here runs on the mesh task inside MyMesh::loop(),
// under the mesh lock; only the outbox is shared, through mesh_ob_enter/exit (a critical section, defined by the
// platform: mesh_task.cpp on the badge, a no-op in the host test).
#include "MyMesh.h"

#include "mesh_frame.h"
#include "mesh_outbox.h"

extern "C" {
void mesh_ob_enter(void);
void mesh_ob_exit(void);
}

#define LINK_PUSH_KINDS 0x11 // push codes 0x80..0x90
#define LINK_FIFO       8

struct LinkStats {
    uint32_t cmds_out = 0, replies = 0, pushes = 0, msg_waiting = 0, drained = 0, no_more = 0;
    uint32_t self_ok = 0, self_bad = 0, lost_replies = 0, fifo_drops = 0, bad_frames = 0, confirmed = 0;
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
    // Queues the start sequence: DEVICE_QUERY(3) so MyMesh builds the V3 message layouts, APP_START, then a drain
    void start();
    LinkStats const &stats() const { return stats_; }
    bool             draining() const { return draining_; }

    void   enable() override {}
    void   disable() override {}
    bool   isEnabled() const override { return true; }
    bool   isConnected() const override { return true; }
    bool   isWriteBusy() const override { return false; }
    size_t writeFrame(uint8_t const src[], size_t len) override;
    size_t checkRecvFrame(uint8_t dest[]) override;

private:
    enum CurKind : uint8_t { CUR_NONE, CUR_INTERNAL, CUR_OUTBOX };
    struct Frame {
        uint8_t len;
        uint8_t data[MF_MAX_FRAME];
    };
    bool   fifo_push(uint8_t const *data, size_t len);
    size_t take_fifo(uint8_t dest[]);
    size_t take_outbox(uint8_t dest[]);
    void   request_drain();
    size_t build_outbox_frame(ob_action_t const &act, uint8_t *dest);
    void   on_reply(mf_msg_t const &m);
    void   on_push(mf_msg_t const &m);

    MyMesh    *mesh_ = nullptr;
    ob_t      *ob_   = nullptr;
    Frame      fifo_[LINK_FIFO];
    uint8_t    fifo_head_ = 0, fifo_n_ = 0;
    CurKind    cur_      = CUR_NONE;
    uint8_t    cur_cmd_  = 0;
    mesh_msg_id_t cur_id_ = 0;
    bool       draining_ = false;
    ob_arb_t   arb_      = {};
    LinkStats  stats_;
    mf_msg_t   scratch_; // off the mesh task's stack: writeFrame runs deep inside loop()
    ob_action_t act_;
};

// MyMesh plus the badge's hooks: the pipe as its interface, and the radio's view of our own packets
class BadgeMesh : public MyMesh {
public:
    BadgeMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore &store,
              ob_t *ob)
        : MyMesh(radio, rng, rtc, tables, store), ob_(ob) {
        pipe_.attach(this, ob);
    }
    CompanionPipe &pipe() { return pipe_; }
    void           startPipe() {
        startInterface(pipe_);
        pipe_.start();
    }

protected:
    void logRxRaw(float snr, float rssi, uint8_t const raw[], int len) override {} // no LOG_RX_DATA push per packet
    void logRx(mesh::Packet *packet, int len, float score) override;
    void logTx(mesh::Packet *packet, int len) override;
    void logTxFail(mesh::Packet *packet, int len) override;

private:
    ob_t         *ob_;
    CompanionPipe pipe_;
};
