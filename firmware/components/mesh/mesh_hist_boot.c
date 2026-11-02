// The boot's replay of the journal and its re-reads (#120, #123). Pure (host-tested in mesh_hist_test.c); mesh_journal.c
// gives it the file reads and the mesh lock.
#include "mesh_hist.h"

#include <string.h>

typedef struct {
    mesh_hist_t       *h;
    mesh_hist_table_t *t;
    uint32_t           max_seq;
    void (*lock)(void);
    void (*unlock)(void);
} replay_ctx_t;

// Every record into the store (under the lock, a record at a time: the screens may read it) and counted, so the
// compaction decision needs no second pass
static bool replay_rec(mesh_hist_rec_t const *r, uint8_t const *raw, size_t bytes, void *ctx) {
    (void)raw;
    replay_ctx_t *rc = (replay_ctx_t *)ctx;
    rc->lock();
    mesh_hist_replay(rc->h, r);
    rc->unlock();
    mesh_hist_count(rc->t, r, bytes);
    if (r->type == MESH_HIST_MSG && r->seq > rc->max_seq) {
        rc->max_seq = r->seq;
    }
    return true;
}

// One whole read, replayed. The counts (table, loaded) start again with each read, so they are the last read's; the
// store keeps what earlier reads put in it, which is harmless: a record only replays when its tag passed, and the store
// takes a seq it already holds as the same message again
static bool read_once(mesh_hist_pass_fn pass, void *pctx, int read_no, replay_ctx_t *rc, mesh_hist_scan_t *scan) {
    mesh_hist_table_init(rc->t, rc->t->slots, rc->t->cap);
    rc->lock();
    rc->h->loaded = 0;
    rc->unlock();
    memset(scan, 0, sizeof(*scan));
    return pass(pctx, read_no, replay_rec, rc, scan) && !scan->crypto_error;
}

void mesh_hist_boot_load(mesh_hist_pass_fn pass, void *pctx, mesh_hist_t *h, mesh_hist_table_t *t, int rereads,
                         void (*lock)(void), void (*unlock)(void), mesh_hist_boot_t *out) {
    memset(out, 0, sizeof(*out));
    replay_ctx_t rc = {.h = h, .t = t, .lock = lock, .unlock = unlock};
    out->reads      = 1;
    bool ok         = read_once(pass, pctx, 1, &rc, &out->first);
    out->last       = out->first;
    out->max_seq    = rc.max_seq;
    if (!ok) {
        out->read_failed = !out->first.crypto_error;
        return;
    }
    // #120: damage in the file reads the same every time; a read error comes and goes, so one clean re-read (the
    // records' checks cannot pass a flipped bit) proves the file is fine. Each re-read replays too (#123): the clean
    // one fills in the records the misread skipped, so the RAM history is whole without a restart.
    bool             damaged = out->first.torn || out->first.resyncs;
    mesh_hist_scan_t prev    = out->first;
    for (int tries = 0; damaged && tries < rereads; tries++) {
        mesh_hist_scan_t again;
        uint32_t         good_loaded = h->loaded; // only the boot writes it; the lock is for the screens' reads
        ok                           = read_once(pass, pctx, ++out->reads, &rc, &again);
        out->max_seq                 = rc.max_seq > out->max_seq ? rc.max_seq : out->max_seq;
        if (!ok) {
            // A re-read that fails (I/O, crypto) proves nothing either way: taking it for damage in the file would
            // compact away records a transient fault hid. No verdict; the last good read's count stands.
            lock();
            h->loaded = good_loaded;
            unlock();
            out->unknown = true;
            return;
        }
        damaged   = mesh_hist_damage_in_file(&out->first, true, &again);
        out->last = again;
        if (damaged && mesh_hist_same_damage(&prev, &again)) {
            break; // the same damage twice in a row: in the file (a torn tail after a power cut), no more reads
        }
        prev = again;
    }
    out->damaged    = damaged;
    out->read_error = !damaged && (out->first.torn || out->first.resyncs);
}
