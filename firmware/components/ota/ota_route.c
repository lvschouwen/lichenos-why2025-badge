#include "ota_route.h"

// Route -> handler. Method and path -> route is the parser's table (ota_proto.c): a request reaches a row only as a
// parsed, admitted route.
static ota_route_row_t const rows[] = {
    [OTA_ROUTE_INFO]       = {.whole = ota_on_info, .up_front = false}, // answered before the refusals, as always
    [OTA_ROUTE_FIRMWARE]   = {.whole = ota_on_firmware, .piece = ota_on_piece, .up_front = true},
    [OTA_ROUTE_FILE]       = {.whole = ota_on_file, .piece = ota_on_piece, .up_front = true},
    [OTA_ROUTE_REBOOT]     = {.whole = ota_on_reboot, .up_front = true},
    [OTA_ROUTE_DELETE]     = {.whole = ota_on_delete, .up_front = true},
    [OTA_ROUTE_PREPARE]    = {.whole = ota_on_prepare, .up_front = true},
    [OTA_ROUTE_ASSETS]     = {.whole = ota_on_assets, .piece = ota_on_piece, .up_front = true},
    [OTA_ROUTE_SCREENSHOT] = {.whole = ota_on_screenshot, .up_front = true},
    [OTA_ROUTE_LOG]        = {.whole = ota_on_log, .up_front = true},
    [OTA_ROUTE_CONSOLE]    = {.whole = ota_on_console, .up_front = true},
    [OTA_ROUTE_CRASHES]    = {.whole = ota_on_crashes, .up_front = true},
};

int ota_request_admit(char const *head, size_t head_len, char const *token, ota_request_t *req, char const **why) {
    int status = ota_parse_request(head, head_len, req);
    if (status) {
        *why = "bad request";
        return status;
    }
    if (ota_route_needs_token(req->route) && (!req->has_token || !ota_token_equal(req->token, token))) {
        *why = "wrong or missing token";
        return 401;
    }
    return 0;
}

ota_route_row_t const *ota_route_row(ota_route_t route) {
    return (size_t)route < sizeof(rows) / sizeof(rows[0]) ? &rows[route] : NULL;
}

ota_route_fn ota_route_open(char const *head, size_t head_len, char const *token, ota_request_t *req,
                            ota_verdict_t *v) {
    v->why      = NULL;
    v->up_front = false;
    v->status   = ota_request_admit(head, head_len, token, req, &v->why);
    if (v->status) {
        return NULL;
    }
    ota_route_row_t const *row = ota_route_row(req->route);
    if (!row || !row->whole) {
        return NULL;
    }
    v->up_front = row->up_front;
    return req->has_offset && row->piece ? row->piece : row->whole;
}
