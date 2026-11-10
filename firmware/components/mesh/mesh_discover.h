#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "Discover nearby" (#167): one zero-hop control request (upstream CMD_SEND_CONTROL_DATA, a NODE_DISCOVER_REQ) that nearby
// repeaters answer, shown live with SNR. Zero hop: it is heard by nodes in direct range and not passed on. Pure logic,
// no ESP headers: built into the host tests. The table is not locked; mesh_task.cpp serialises (a mutex, #271).

#define DISC_PREFIX     8    // bytes of the answerer's public key kept (the request asks for the 8-byte prefix form)
#define DISC_NAME_MAX   32
#define DISC_MAX        24   // answers kept; later ones are counted but not listed
#define DISC_WINDOW_S   15   // how long answers are taken after the request
#define DISC_GAP_S      60   // at least this long between two requests
#define DISC_REQ_LEN    6
#define DISC_REQ_TYPE   0x81 // CTL_TYPE_NODE_DISCOVER_REQ | prefix only
#define DISC_RESP_TYPE  0x90 // CTL_TYPE_NODE_DISCOVER_RESP, low 4 bits the answerer's type
#define DISC_TYPE_ALL   0xFF // every node type may answer

typedef enum {
    DISC_IDLE,      // nothing asked since boot
    DISC_PENDING,   // asked; the mesh task has not put it on air yet
    DISC_LISTENING, // on air, taking answers
    DISC_DONE,      // the window ended
    DISC_FAILED,    // the request could not be sent
} disc_state_t;

typedef struct {
    uint8_t  prefix[DISC_PREFIX];
    uint8_t  type;        // ADV_TYPE_*: 1 chat, 2 repeater, 3 room, 4 sensor
    int16_t  snr_x10;     // dB * 10 as we heard the answer
    int8_t   their_snr_x4; // dB * 4 as the answerer heard our request
    uint32_t at_s;        // uptime when the (latest) answer came
    char     name[DISC_NAME_MAX]; // from our contacts, "" when it is not one
} disc_answer_t;

typedef struct {
    disc_state_t  state;
    uint32_t      tag;
    uint32_t      asked_s;    // uptime of the request
    bool          have_asked; // a request was made since boot (asked_s is valid)
    char const   *why;        // DISC_FAILED: static text
    uint16_t      extra;      // answers beyond DISC_MAX
    size_t        count;
    disc_answer_t a[DISC_MAX];
} disc_t;

void disc_init(disc_t *d);
// Seconds still to wait before another request is allowed, 0 when one may go now
uint32_t disc_wait_s(disc_t const *d, uint32_t now_s);
// Starts a request with this tag: clears the answers. false (with *why) when the gap has not passed or one is running.
bool disc_start(disc_t *d, uint32_t tag, uint32_t now_s, char const **why);
// The request's payload for this tag
void disc_request(uint32_t tag, uint8_t out[DISC_REQ_LEN]);
// The mesh task put it on air: the window starts now
void disc_sent(disc_t *d, uint32_t now_s);
// It could not go (no packet to send)
void disc_failed(disc_t *d, char const *why);
// One control packet's payload (as MeshCore hands it over) with the SNR it was heard at. true when it was an answer to
// our request and was kept or counted; a repeated answer from the same node updates its row. name may be NULL.
bool disc_on_payload(disc_t *d, uint8_t const *payload, size_t len, int16_t snr_x10, char const *name, uint32_t now_s);
// Ends the window by time; returns the state
disc_state_t disc_poll(disc_t *d, uint32_t now_s);
// Answers newest first (the latest answer on top); returns how many
size_t disc_copy(disc_t const *d, disc_answer_t *out, size_t max);

// "Listening, 12 s left" / "Heard 3" / "No answers. Nobody nearby, or out of range." / "Not asked yet"
void disc_status(disc_t const *d, uint32_t now_s, char *out, size_t cap);
// The row's first line: the contact's name, else "Repeater 1a2b3c4d"
void disc_row_title(disc_answer_t const *a, char *out, size_t cap);
// The second line: "Repeater, SNR 6.5 dB, hears us at 4.0 dB, 3 s ago"
void disc_row_detail(disc_answer_t const *a, uint32_t now_s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
