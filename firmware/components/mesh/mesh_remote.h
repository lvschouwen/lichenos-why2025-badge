#pragma once
// Log in, status and CLI for one repeater or room (#51). ESP-free, without a clock of its own (now_ms is passed in,
// 32-bit wrap safe) and not locked: on the badge every call is wrapped in mesh_ob_enter/exit, as for mesh_trace.h.
//
// One request at a time: the UI (or the console) asks with rr_request, the mesh task's pipe takes it (rr_take), builds
// MyMesh's command frame and reports MyMesh's reply (rr_on_reply, RESP_SENT), then the push that ends it (login
// success/fail, status response) or, for the CLI, the CLI_DATA message that comes back through MyMesh's offline queue
// (rr_on_message). The UI polls rr_poll, which also ends a request that got no answer in time.
//
// Nothing here sends by itself: a request exists only because rr_request was called, and the screens call it only on
// an Enter of the person (login, command) or when the Status screen is opened. The password lives in the request until
// the pipe takes it, then it is wiped; it is never logged and never part of a result.
#include "mesh_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RR_CMD_MAX    100 // a command typed on the badge (MF_MAX_TEXT is the frame's limit)
#define RR_REPLY_MAX  161
#define RR_ARG_MAX    (RR_CMD_MAX + 4) // what rr_take may hand out: a command and the free CLI's "XY|" tag
#define RR_CLI_KEEP   6   // exchanges kept for the CLI screen
#define RR_GRACE_MS   5000
#define RR_FLOOR_MS   8000
#define RR_CAP_MS     90000
#define RR_APPEND_MS  4000 // a second CLI_DATA message this soon after the first belongs to the same reply

typedef enum { RR_LOGIN, RR_STATUS, RR_CLI, RR_TELEMETRY } rr_op_t; // RR_TELEMETRY (#162): any contact, not only a repeater
typedef enum {
    RR_IDLE,    // nothing asked since boot
    RR_QUEUED,  // waiting for the mesh task
    RR_TAKEN,   // the frame went to MyMesh, its reply not in yet
    RR_SENT,    // MyMesh sent it (RESP_SENT): waiting for the answer
    RR_DONE,    // the answer came
    RR_FAILED,  // MyMesh refused it, the contact is gone, or the login was refused
    RR_TIMEOUT, // no answer in time
} rr_state_t;

typedef enum { RR_CLI_WAITING, RR_CLI_GOT, RR_CLI_NONE } rr_cli_state_t;
typedef struct {
    uint8_t  state; // rr_cli_state_t
    bool     secret; // a password command (mesh_rset.h): cmd and reply are kept without it
    uint32_t got_ms;
    char     tag[3];   // the "XY" this free CLI command went out with (#123), "" when it carries its own or none
    char     cmd[RR_CMD_MAX + 1];
    char     reply[RR_REPLY_MAX];
} rr_exchange_t;

typedef struct {
    uint32_t   seq; // the request's number, 0 = none
    rr_op_t    op;
    rr_state_t state;
    uint8_t    prefix[MF_PREFIX];
    char       arg[RR_CMD_MAX + 1]; // the password (wiped when taken) or the command
    uint8_t    flood;
    uint8_t    err;        // RR_FAILED: MyMesh's ERR code, 0 when the pipe gave up
    bool       refused;    // RR_FAILED: the server said no (wrong password)
    char const *why;       // RR_FAILED: a short reason, static text
    uint32_t   asked_ms, sent_ms, est_ms, done_ms;
    bool       secret;     // arg is a password (a login, or a password command): wiped once taken
    // the session: whom we are logged in to (one at a time; a new login to someone else ends it)
    bool       logged_in, admin;
    uint8_t    login_prefix[MF_PREFIX];
    uint8_t    acl, fw_level;
    // the last status
    bool        have_status;
    uint8_t     status_prefix[MF_PREFIX];
    mf_status_t status;
    // the last telemetry answer (#162): the Cayenne LPP bytes as the contact sent them
    bool    have_telem;
    uint8_t telem_prefix[MF_PREFIX];
    uint8_t telem_len;
    uint8_t telem[MF_TELEM_MAX];
    // the CLI exchanges with cli_prefix, newest at cli[cli_head]
    uint8_t       cli_prefix[MF_PREFIX];
    uint8_t       cli_n, cli_head;
    rr_exchange_t cli[RR_CLI_KEEP];
    // a repeater that did not reflect our tag ("Unknown command" without one): its commands go untagged from then on
    bool    notag;
    uint8_t notag_prefix[MF_PREFIX];
} rr_t;

// What rr_request keeps of a command, worked out before the caller enters its lock (mesh_ob_enter is a critical
// section): the command's class (mesh_rset.h) and the text the CLI history shows (a password hidden)
typedef struct {
    uint8_t cls; // rs_cmd_class_t
    char    shown[RR_CMD_MAX + 1];
} rr_prep_t;
void rr_prepare(rr_op_t op, char const *arg, rr_prep_t *p);

// What a poller needs of the request (the settings page, #117): far smaller than rr_t, so it is copied in a critical
// section without the CLI history
typedef struct {
    uint32_t      seq;
    rr_op_t       op;
    rr_state_t    state;
    char const   *why;
    bool          logged_in, admin;
    uint8_t       login_prefix[MF_PREFIX];
    uint8_t       cli_prefix[MF_PREFIX];
    uint8_t       cli_n;
    rr_exchange_t cli; // the newest exchange
    // for the status history (#168): whom the request is to, how long ago it was asked, and the last status answer
    uint8_t     prefix[MF_PREFIX];
    uint32_t    age_ms;
    bool        have_status;
    uint8_t     status_prefix[MF_PREFIX];
    mf_status_t status;
} rr_head_t;

void rr_init(rr_t *r);
bool rr_busy(rr_t const *r);
// A new request: its seq, or 0 with *why set (one is still running, empty or too long, or a command the badge never
// sends: erase, mesh_rset.h). arg: the password (login, at
// most MF_PASSWORD_MAX characters) or the command; ignored for a status request.
uint32_t rr_request(rr_t *r, rr_op_t op, uint8_t const prefix[MF_PREFIX], char const *arg, uint32_t now_ms,
                    char const **why);
// rr_request with the command prepared (rr_prepare) beforehand: what the badge runs inside its lock
uint32_t rr_request_prepared(rr_t *r, rr_op_t op, uint8_t const prefix[MF_PREFIX], char const *arg,
                             rr_prep_t const *p, uint32_t now_ms, char const **why);
// The mesh task: the queued request, now marked taken; false when none is queued. The password is copied to arg_out
// (cap bytes, RR_ARG_MAX fits everything) and wiped from r. A free CLI command gets a two-letter tag ("QX|get name"):
// simple_repeater and the room server reflect it, and rr_on_message drops a reply that carries another one, so a late
// reply to an earlier (timed-out) command is never shown as the answer to this one (#123).
bool rr_take(rr_t *r, rr_op_t *op, uint8_t prefix[MF_PREFIX], char *arg_out, size_t cap, uint32_t now_ms);
// The taken request could not be sent (contact gone, no frame): why is static text
void rr_fail(rr_t *r, char const *why);
// MyMesh's reply to the taken request's frame: SENT moves on, anything else fails it
void rr_on_reply(rr_t *r, mf_msg_t const *m, uint32_t now_ms);
// PUSH_LOGIN_SUCCESS / PUSH_LOGIN_FAIL / PUSH_STATUS_RESPONSE: true when they answer the running request
bool rr_on_login(rr_t *r, mf_msg_t const *m, uint32_t now_ms);
bool rr_on_status(rr_t *r, mf_msg_t const *m, uint32_t now_ms);
// PUSH_TELEMETRY_RESPONSE (#162): true when it answers the running telemetry request
bool rr_on_telemetry(rr_t *r, mf_msg_t const *m, uint32_t now_ms);
// A contact message from the drained offline queue: true when it is the CLI reply (CLI_DATA) we wait for or a
// following part of it
bool rr_on_message(rr_t *r, mf_msg_t const *m, uint32_t now_ms);
// Ends the session with this contact (Log out): local, nothing goes on air
void rr_logout(rr_t *r, uint8_t const prefix[MF_PREFIX]);
bool rr_logged_in(rr_t const *r, uint8_t const prefix[MF_PREFIX]);
// How long the answer may take after RESP_SENT: MyMesh's estimate plus RR_GRACE_MS, within RR_FLOOR_MS..RR_CAP_MS
uint32_t rr_deadline_ms(uint32_t est_ms);
// Times out a request that waited too long, then copies it to out (may be NULL)
void rr_poll(rr_t *r, uint32_t now_ms, rr_t *out);
// rr_poll with the small copy
void rr_poll_head(rr_t *r, uint32_t now_ms, rr_head_t *out);
bool rr_head_logged_in(rr_head_t const *h, uint8_t const prefix[MF_PREFIX]);
// rr_busy for the small copy: queued, taken or sent
bool rr_head_busy(rr_head_t const *h);

// The NVS key the contact's remembered password is kept under, in the app's settings: "pw" and the 12 hex digits of the
// key prefix (14 characters, NVS allows 15). Empty out when cap is under RR_PW_KEY_MAX.
#define RR_PW_KEY_MAX 15
void rr_pw_key(uint8_t const prefix[MF_PREFIX], char *out, size_t cap);

// ---- text for the screens and the console ------------------------------------------------------------------------
#define RR_LINE_MAX  64
#define RR_LINES_MAX 16
typedef struct {
    char text[RR_LINE_MAX];
} rr_line_t;
// The status as lines ("Uptime: 3d 4h 5m", "Battery: 4.12 V", "Queue: 0", ...); returns how many
size_t rr_status_lines(mf_status_t const *s, rr_line_t *out, size_t max);
// "3d 4h 5m", "4h 5m", "5m 10s", "10s"
void rr_format_uptime(uint32_t secs, char *out, size_t cap);
// One line for a request that is not (successfully) done, or for the running one: "Logging in...", "Wrong password",
// "No answer from the repeater (are you logged in?)"
void rr_state_text(rr_t const *r, char *out, size_t cap);
// The CLI exchanges, newest first, for a scrolling label: "> cmd" then its reply or "(waiting...)" / "(no reply)",
// separated by a blank line. Returns the length written.
size_t rr_cli_format(rr_t const *r, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
