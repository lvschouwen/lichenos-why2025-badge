#pragma once
// The update server's front door (#447): a request head is parsed and its token checked here, the one place every route
// passes before a handler runs, and the route table names each route's handler. Pure (no ESP headers), host-tested in
// host_tests/ota_route_test.c, which also proves no row of the table is reached without the token.

#include "ota_proto.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Parse a complete request head and check its token against token, the server's own (wiped to "" while Wi-Fi updates
// are refused, which no request matches). 0: admitted. Else the status to answer with and *why the reply's text: the
// parser's status with "bad request", or 401 "wrong or missing token".
int ota_request_admit(char const *head, size_t head_len, char const *token, ota_request_t *req, char const **why);

// One admitted request on its connection: pre is what was read behind the head (the body's first pre_len bytes)
typedef struct {
    int                  fd;
    ota_request_t const *req;
    uint8_t const       *pre;
    size_t               pre_len;
} ota_conn_t;

// A route's handler: answers the request itself; true when the badge should reboot afterwards
typedef bool (*ota_route_fn)(ota_conn_t const *conn);

// A row of the route table. piece: the handler of a request with X-Offset (NULL: whole for it too). up_front: the trial
// and USB-hold refusals (ota_server.c) are asked before the handler; only /info skips them.
typedef struct {
    ota_route_fn whole;
    ota_route_fn piece;
    bool         up_front;
} ota_route_row_t;

// The row of route; NULL for a value outside the table
ota_route_row_t const *ota_route_row(ota_route_t route);

// What ota_route_open decided: status 0 or the one to answer with, why its text, up_front the row's
typedef struct {
    int         status;
    char const *why;
    bool        up_front;
} ota_verdict_t;

// The only way to a handler: ota_request_admit, then the table. NULL with a status (and why) when refused; NULL with
// status 0 for a route without a row (none: the test checks every route has one). Else the handler. Either way the
// head's head_len bytes and req->token are wiped after the check (#456); req->has_token stays.
ota_route_fn ota_route_open(char *head, size_t head_len, char const *token, ota_request_t *req, ota_verdict_t *v);

// The handlers, in ota_server.c (prepare, reboot), ota_upload.c, ota_piece_http.c and ota_extra.c
bool ota_on_info(ota_conn_t const *conn);
bool ota_on_firmware(ota_conn_t const *conn);
bool ota_on_file(ota_conn_t const *conn);
bool ota_on_piece(ota_conn_t const *conn);
bool ota_on_assets(ota_conn_t const *conn);
bool ota_on_prepare(ota_conn_t const *conn);
bool ota_on_delete(ota_conn_t const *conn);
bool ota_on_screenshot(ota_conn_t const *conn);
bool ota_on_log(ota_conn_t const *conn);
bool ota_on_crashes(ota_conn_t const *conn);
bool ota_on_console(ota_conn_t const *conn);
bool ota_on_reboot(ota_conn_t const *conn);
