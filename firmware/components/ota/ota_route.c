#include "ota_route.h"

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
