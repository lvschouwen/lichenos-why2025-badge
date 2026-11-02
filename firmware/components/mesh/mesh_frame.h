#pragma once
// MeshCore's companion frames (#45): the few commands the badge sends to its own MyMesh through the in-RAM pipe, and the
// replies and pushes it reads back. ESP-free, so host_tests/mesh_frame_test.c and companion_test.cpp pin the numbers
// (MyMesh.cpp keeps them as #defines inside the .cpp, so this is our copy).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MF_MAX_FRAME 176 // MAX_FRAME_SIZE: MyMesh's buffers are one byte bigger, never write more than this
#define MF_MAX_TEXT  160 // MAX_TEXT_LEN
#define MF_NAME_MAX  32
#define MF_PUB_LEN   32
#define MF_PREFIX    6

enum {
    MF_CMD_APP_START          = 1,
    MF_CMD_SEND_TXT_MSG       = 2,
    MF_CMD_SEND_CHANNEL_TXT   = 3,
    MF_CMD_ADD_UPDATE_CONTACT = 9,
    MF_CMD_SYNC_NEXT          = 10,
    MF_CMD_RESET_PATH         = 13,
    MF_CMD_SEND_LOGIN         = 26,
    MF_CMD_SEND_STATUS_REQ    = 27,
    MF_CMD_LOGOUT             = 29,
    MF_CMD_REMOVE_CONTACT     = 15,
    MF_CMD_DEVICE_QUERY       = 22,
    MF_CMD_SEND_TRACE_PATH    = 36,
    MF_CMD_PATH_DISCOVERY     = 52, // CMD_SEND_PATH_DISCOVERY_REQ
};
enum {
    MF_RESP_OK                  = 0,
    MF_RESP_ERR                 = 1,
    MF_RESP_SELF_INFO           = 5,
    MF_RESP_SENT                = 6,
    MF_RESP_CONTACT_MSG_RECV    = 7,
    MF_RESP_CHANNEL_MSG_RECV    = 8,
    MF_RESP_NO_MORE_MESSAGES    = 10,
    MF_RESP_DEVICE_INFO         = 13,
    MF_RESP_CONTACT_MSG_RECV_V3 = 16,
    MF_RESP_CHANNEL_MSG_RECV_V3 = 17,
    MF_PUSH_PATH_UPDATED        = 0x81,
    MF_PUSH_SEND_CONFIRMED      = 0x82,
    MF_PUSH_MSG_WAITING         = 0x83,
    MF_PUSH_LOGIN_SUCCESS       = 0x85,
    MF_PUSH_LOGIN_FAIL          = 0x86,
    MF_PUSH_STATUS_RESPONSE     = 0x87,
    MF_PUSH_TRACE_DATA          = 0x89,
    MF_PUSH_PATH_DISCOVERY      = 0x8D, // PUSH_CODE_PATH_DISCOVERY_RESPONSE
};
enum { MF_ERR_NOT_FOUND = 2, MF_ERR_TABLE_FULL = 3 };
// The text types of a contact message (TxtDataHelpers.h): a repeater's CLI reply is CLI_DATA, a room's post SIGNED_PLAIN
enum { MF_TXT_PLAIN = 0, MF_TXT_CLI_DATA = 1, MF_TXT_SIGNED_PLAIN = 2, MF_TXT_CLI_COMMAND = 3 };
#define MF_PASSWORD_MAX 15 // sendLogin keeps 15 characters of the password

typedef enum {
    MF_BAD,     // a known code, but the frame is too short
    MF_UNKNOWN, // a code we do not use: not an error
    MF_OK,
    MF_ERR,
    MF_SENT,
    MF_DEVICE_INFO,
    MF_SELF_INFO,
    MF_NO_MORE,
    MF_MSG_WAITING,
    MF_MSG_RECV,
    MF_SEND_CONFIRMED,
    MF_PATH_UPDATED,
    MF_LOGIN_SUCCESS,
    MF_LOGIN_FAIL,
    MF_STATUS,
    MF_TRACE,     // PUSH_TRACE_DATA: a trace came back (#53)
    MF_DISCOVERY, // PUSH_PATH_DISCOVERY_RESPONSE: the contact answered a path discovery (#53)
} mf_kind_t;

#define MF_PATH_MAX       64  // MAX_PATH_SIZE
#define MF_TRACE_HASH_MAX 128 // a trace's hash list: up to 64 hops of 1- or 2-byte hashes (MyMesh refuses > 64 hops)

// A trace's result (#53), as MyMesh::onTraceRecv pushes it: the hashes the trace was sent along, the SNR each hop heard
// it with (dB x 4, signed), and the SNR we heard the last hop with
typedef struct {
    uint32_t tag, auth;
    uint8_t  flags;     // bits 0-1: log2 of the bytes per hash
    uint8_t  hash_len;  // bytes in hashes
    uint8_t  hops;      // hash_len >> (flags & 3): one SNR per hop
    uint8_t  hashes[MF_TRACE_HASH_MAX];
    int8_t   snr_q[MF_PATH_MAX];
    int8_t   final_snr_q;
} mf_trace_t;

// A path discovery's answer (#53): the path to the contact (to store) and the one its answer came back along, both
// packed as ContactInfo.out_path_len (hop count in bits 0-5, hash size - 1 in bits 6-7)
typedef struct {
    uint8_t prefix[6];
    uint8_t out_len, in_len;
    uint8_t out_path[MF_PATH_MAX];
    uint8_t in_path[MF_PATH_MAX];
} mf_discovery_t;

// A repeater's status (its RepeaterStats, as PUSH_STATUS_RESPONSE carries it, 56 bytes): a shorter answer from an older
// repeater leaves the rest at 0, `bytes` says how much there was
#define MF_STATUS_BYTES 56
typedef struct {
    uint8_t  bytes;
    uint16_t batt_mv, tx_queue;
    int16_t  noise_floor, last_rssi;
    uint32_t n_recv, n_sent, air_secs, up_secs;
    uint32_t sent_flood, sent_direct, recv_flood, recv_direct;
    uint16_t err_events;
    int16_t  last_snr_q; // dB x 4
    uint16_t direct_dups, flood_dups;
    uint32_t rx_air_secs, recv_errors;
} mf_status_t;

typedef struct {
    mf_kind_t kind;
    uint8_t   code; // the frame's first byte
    uint8_t   err;  // MF_ERR
    uint8_t   flood; // MF_SENT
    uint8_t   fw_ver; // MF_DEVICE_INFO
    bool      is_channel; // MF_MSG_RECV
    uint32_t  ack;        // MF_SENT, MF_SEND_CONFIRMED
    uint32_t  est_timeout; // MF_SENT, ms
    uint32_t  trip_ms;    // MF_SEND_CONFIRMED
    uint8_t   pub[MF_PUB_LEN]; // MF_SELF_INFO
    char      name[MF_NAME_MAX]; // MF_SELF_INFO
    mf_trace_t     trace; // MF_TRACE
    mf_discovery_t disc;  // MF_DISCOVERY
    // MF_LOGIN_SUCCESS / MF_LOGIN_FAIL / MF_STATUS (and MF_MSG_RECV from a contact): who it is about
    uint8_t   rprefix[MF_PREFIX];
    uint8_t   admin, acl, fw_level; // MF_LOGIN_SUCCESS: permissions bit 0 = admin; acl and fw_level only from newer servers
    mf_status_t status;             // MF_STATUS
    // MF_MSG_RECV from a contact (not a channel): msg_ok when the frame was whole
    bool      msg_ok;
    uint8_t   txt_type, author[4]; // author: a room post's sender (txt_type SIGNED_PLAIN)
    char      text[MF_MAX_TEXT + 1];
} mf_msg_t;

// Encoders: the length written to out (>= MF_MAX_FRAME bytes), 0 when the frame would not fit or the text is too long
size_t mf_encode_device_query(uint8_t *out, uint8_t app_ver);
size_t mf_encode_app_start(uint8_t *out, char const *app_name);
size_t mf_encode_sync_next(uint8_t *out);
size_t mf_encode_send_channel(uint8_t *out, uint8_t channel_idx, uint32_t timestamp, char const *text, size_t text_len);
size_t mf_encode_send_dm(uint8_t *out, uint8_t attempt, uint32_t timestamp, uint8_t const prefix[MF_PREFIX],
                         char const *text, size_t text_len);
// CMD_SEND_TXT_MSG with the CLI text type: a command for a repeater we are logged in to (its reply comes back as a
// CLI_DATA message)
size_t mf_encode_send_cli(uint8_t *out, uint8_t attempt, uint32_t timestamp, uint8_t const prefix[MF_PREFIX],
                          char const *text, size_t text_len);
// CMD_SEND_LOGIN: the password is cut at MF_PASSWORD_MAX characters by MyMesh, so more is refused here (0)
size_t mf_encode_login(uint8_t *out, uint8_t const pub[MF_PUB_LEN], char const *password);
size_t mf_encode_status_req(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);
size_t mf_encode_logout(uint8_t *out, uint8_t const pub[MF_PUB_LEN]); // local: MyMesh stops its keep-alives to the server
size_t mf_encode_reset_path(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);
size_t mf_encode_remove_contact(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);
// CMD_SEND_TRACE_PATH (#53): the trace goes along hashes (hash_len bytes, each 1 << (flags & 3) bytes), every repeater
// on it adds the SNR it heard. 0 when the list is empty, not whole hashes, or over 64 hops.
size_t mf_encode_trace(uint8_t *out, uint32_t tag, uint32_t auth, uint8_t flags, uint8_t const *hashes, size_t hash_len);
// CMD_SEND_PATH_DISCOVERY_REQ (#53): a flooded request the contact answers along the path it learned
size_t mf_encode_discovery(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);

// A whole contact for CMD_ADD_UPDATE_CONTACT (#49), as MyMesh::updateContactFromFrame reads it. lastmod goes along so
// a changed flag does not move the contact in "most recently heard" order.
typedef struct {
    uint8_t  pub[MF_PUB_LEN];
    uint8_t  type, flags, out_path_len;
    uint8_t  out_path[MF_PATH_MAX];
    char     name[MF_NAME_MAX]; // not necessarily terminated, as ContactInfo.name
    uint32_t last_advert_timestamp, lastmod;
    int32_t  gps_lat, gps_lon;
} mf_contact_t;
size_t mf_encode_update_contact(uint8_t *out, mf_contact_t const *c); // 148 bytes

void mf_decode(uint8_t const *frame, size_t len, mf_msg_t *out);

// text cut to at most max bytes without splitting a UTF-8 character; returns the new length
size_t mf_text_cut(char const *text, size_t max);
// What fits in a channel message: sendGroupMessage sends "<node name>: <text>" in MF_MAX_TEXT bytes
size_t mf_channel_text_max(size_t node_name_len);

#ifdef __cplusplus
}
#endif
