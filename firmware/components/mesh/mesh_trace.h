#pragma once
// Trace path and path discovery for one contact (#53). ESP-free, without a clock of its own (now_ms is passed in, 32-bit
// wrap safe) and not locked: on the badge every call is wrapped in mesh_ob_enter/exit, as for the outbox.
//
// One request at a time: the UI (or the console) asks with mt_request, the mesh task's pipe takes it (mt_take), builds
// MyMesh's command frame and reports MyMesh's reply (mt_on_reply), then the push that ends it (mt_on_trace /
// mt_on_discovery). The UI polls mt_poll, which also ends a request that got no answer in time.
//
// A trace goes along the stored path and back. Only repeaters pass a trace on (Mesh::onRecvPacket), so the trace of a
// repeater goes through it and back (path + its own hash + path reversed); for anyone else it goes to the last repeater
// on the path and back, which shows the path but not the contact's own link.
#include "mesh_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT_PATH_UNKNOWN 0xFF // ContactInfo.out_path_len: no path, MeshCore floods
#define MT_ADV_REPEATER 2    // ADV_TYPE_REPEATER
#define MT_GRACE_MS     3000  // on top of MyMesh's own estimate: a busy channel, the repeaters' random delays
#define MT_FLOOR_MS     5000
#define MT_CAP_MS       60000

typedef enum { MT_TRACE, MT_DISCOVER } mt_kind_t;
typedef enum {
    MT_IDLE,    // nothing asked since boot
    MT_QUEUED,  // waiting for the mesh task
    MT_TAKEN,   // the frame went to MyMesh, its reply not in yet
    MT_SENT,    // MyMesh sent it (RESP_SENT): waiting for the answer
    MT_DONE,    // the answer came
    MT_FAILED,  // MyMesh refused it, or the contact is gone
    MT_TIMEOUT, // no answer in time
} mt_state_t;

// The hashes a trace goes along, from the contact's stored path
typedef struct {
    uint8_t flags;    // bits 0-1: log2 of the bytes per hash
    uint8_t hash_len; // bytes
    int8_t  target;   // the hop that is the contact itself (a repeater), -1 when the trace does not reach it
    uint8_t hashes[MF_TRACE_HASH_MAX];
} mt_plan_t;
// NULL when out holds the plan, else why there is none ("no path known: Discover path first", ...). direct_hash_size
// (1 or 2: our path hash setting) is the repeater's hash size when it is a direct neighbour (its path has no hashes).
char const *mt_plan(uint8_t type, uint8_t path_len, uint8_t const *path, uint8_t const pub[MF_PUB_LEN],
                    uint8_t direct_hash_size, mt_plan_t *out);

typedef struct {
    uint32_t       seq; // the request's number, 0 = none
    mt_kind_t      kind;
    mt_state_t     state;
    uint8_t        prefix[MF_PREFIX];
    uint32_t       tag;     // the trace's tag (chosen by the asker); MyMesh's request tag for a discovery
    uint8_t        flood;   // MF_SENT: MyMesh flooded it
    uint8_t        err;     // MT_FAILED: MyMesh's ERR code, 0 when the pipe gave up (contact gone, no frame)
    char const    *why;     // MT_FAILED: a short reason, static text
    int8_t         target;  // as mt_plan_t (the trace's), -1 for a discovery
    bool           console; // asked for by a console line (#290): a discovery floods with the console scope
    uint32_t       asked_ms, sent_ms, on_air_ms, done_ms, est_ms;
    mf_trace_t     trace; // MT_DONE, MT_TRACE
    mf_discovery_t disc;  // MT_DONE, MT_DISCOVER
} mt_t;

void mt_init(mt_t *t);
// A new request: its seq, or 0 with *why set (one is still running). tag is the trace's (random, from the asker).
uint32_t mt_request(mt_t *t, mt_kind_t kind, uint8_t const prefix[MF_PREFIX], uint32_t tag, uint32_t now_ms,
                    char const **why);
// The mesh task: the queued request (its kind, prefix and tag), now marked taken; false when none is queued
bool mt_take(mt_t *t, mt_kind_t *kind, uint8_t prefix[MF_PREFIX], uint32_t *tag, uint32_t now_ms);
// The taken request could not be sent (contact gone, no path to trace): why is static text
void mt_fail(mt_t *t, char const *why);
// The taken request's plan target (a trace's hop that is the contact)
void mt_set_target(mt_t *t, int8_t target);
// MyMesh's reply to the taken request's frame: SENT moves on, anything else fails it
void mt_on_reply(mt_t *t, mf_msg_t const *m, uint32_t now_ms);
// Our trace went on air (BadgeMesh::logTx sees its tag): the round trip counts from here
void mt_on_tx(mt_t *t, uint32_t tag, uint32_t now_ms);
// A trace came back: true when it is ours (the tag), now done
bool mt_on_trace(mt_t *t, mf_trace_t const *tr, uint32_t now_ms);
// A discovery answer: true when it is ours (the contact), now done; the caller stores the path
bool mt_on_discovery(mt_t *t, mf_discovery_t const *d, uint32_t now_ms);
// How long the answer may take after RESP_SENT: MyMesh's estimate plus MT_GRACE_MS, within MT_FLOOR_MS..MT_CAP_MS
uint32_t mt_deadline_ms(uint32_t est_ms);
// Times out a request that waited too long, then copies it to out (may be NULL)
void mt_poll(mt_t *t, uint32_t now_ms, mt_t *out);
// The round trip in ms (from on air when that was seen, else from MyMesh taking it); 0 when not done
uint32_t mt_round_trip_ms(mt_t const *t);
bool     mt_busy(mt_t const *t);

// The result screen's and the console's lines. Each fits in MT_LINE_MAX (the console prints them one by one).
#define MT_LINE_MAX  64
#define MT_LINES_MAX 72 // a heading, every hop (up to 64), the last one, the round trip, a note
typedef struct {
    char text[MT_LINE_MAX];
} mt_line_t;
// "Waiting for the answer...", the hops with their SNR ("1. 2ae9  SNR 7.25 dB"), "Back here: SNR 9.5 dB", "Round trip:
// 1234 ms", or why it failed. Returns how many lines.
size_t mt_format(mt_t const *t, mt_line_t *out, size_t max);
// SNR in quarter dB as text: "7.25", "-3.5", "0"
void mt_format_snr(int8_t q, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
