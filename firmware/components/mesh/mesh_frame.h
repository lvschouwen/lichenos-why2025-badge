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
    MF_CMD_APP_START        = 1,
    MF_CMD_SEND_TXT_MSG     = 2,
    MF_CMD_SEND_CHANNEL_TXT = 3,
    MF_CMD_ADD_UPDATE_CONTACT = 9,
    MF_CMD_SYNC_NEXT        = 10,
    MF_CMD_RESET_PATH       = 13,
    MF_CMD_REMOVE_CONTACT   = 15,
    MF_CMD_DEVICE_QUERY     = 22,
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
};
enum { MF_ERR_NOT_FOUND = 2, MF_ERR_TABLE_FULL = 3 };

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
} mf_kind_t;

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
} mf_msg_t;

// Encoders: the length written to out (>= MF_MAX_FRAME bytes), 0 when the frame would not fit or the text is too long
size_t mf_encode_device_query(uint8_t *out, uint8_t app_ver);
size_t mf_encode_app_start(uint8_t *out, char const *app_name);
size_t mf_encode_sync_next(uint8_t *out);
size_t mf_encode_send_channel(uint8_t *out, uint8_t channel_idx, uint32_t timestamp, char const *text, size_t text_len);
size_t mf_encode_send_dm(uint8_t *out, uint8_t attempt, uint32_t timestamp, uint8_t const prefix[MF_PREFIX],
                         char const *text, size_t text_len);
size_t mf_encode_reset_path(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);
size_t mf_encode_remove_contact(uint8_t *out, uint8_t const pub[MF_PUB_LEN]);

// A whole contact for CMD_ADD_UPDATE_CONTACT (#49), as MyMesh::updateContactFromFrame reads it. lastmod goes along so
// a changed flag does not move the contact in "most recently heard" order.
#define MF_PATH_MAX 64 // MAX_PATH_SIZE
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
