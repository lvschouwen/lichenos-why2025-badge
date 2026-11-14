#pragma once
// The update server's front door (#447): a request head is parsed and its token checked here, the one place every route
// passes before a handler runs. Pure (no ESP headers), host-tested in host_tests/ota_route_test.c.

#include "ota_proto.h"

#include <stddef.h>

// Parse a complete request head and check its token against token, the server's own (wiped to "" while Wi-Fi updates
// are refused, which no request matches). 0: admitted. Else the status to answer with and *why the reply's text: the
// parser's status with "bad request", or 401 "wrong or missing token".
int ota_request_admit(char const *head, size_t head_len, char const *token, ota_request_t *req, char const **why);
