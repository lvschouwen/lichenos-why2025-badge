// Pins of the update server's front door (#447), recorded from the code before its split: every route with the right,
// a wrong, a malformed and no token, each wrong method, unknown targets and malformed heads, plus seeded mutations of
// the recorded heads under ASan+UBSan. The token check must hold for every route the parser knows. Since the split the
// route table is pinned too: every route reaches the handler the old switch ran, and none without the token.
#include "mutate.h"
#include "ota_route.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

// Stand-ins for the handlers (ota_upload.c, ota_piece_http.c, ota_extra.c, ota_server.c): each records that it ran
static char const *ran;
#define STUB(name, reboot)                                                                                             \
    bool name(ota_conn_t const *conn) {                                                                                \
        (void)conn;                                                                                                    \
        ran = #name;                                                                                                   \
        return reboot;                                                                                                 \
    }
STUB(ota_on_info, false)
STUB(ota_on_firmware, false)
STUB(ota_on_file, false)
STUB(ota_on_piece, false)
STUB(ota_on_assets, false)
STUB(ota_on_prepare, false)
STUB(ota_on_delete, false)
STUB(ota_on_screenshot, false)
STUB(ota_on_log, false)
STUB(ota_on_crashes, false)
STUB(ota_on_console, false)
STUB(ota_on_reboot, true)

#define TOKEN "0123456789abcdef0123456789abcdef"
#define OTHER "0123456789abcdef0123456789abcdee"
#define SHA   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

// Every route the parser knows, as tools/badge.sh asks for it
static struct {
    char const *method;
    char const *target;
    ota_route_t route;
    char const *whole;    // the handler the old switch in handle_connection ran
    char const *piece;    // the same with X-Offset
    bool        up_front; // whether refused_up_front ran before it (all but /info)
} const routes[] = {
    {"GET", "/info", OTA_ROUTE_INFO, "ota_on_info", "ota_on_info", false},
    {"PUT", "/firmware", OTA_ROUTE_FIRMWARE, "ota_on_firmware", "ota_on_piece", true},
    {"PUT", "/file", OTA_ROUTE_FILE, "ota_on_file", "ota_on_piece", true},
    {"POST", "/reboot", OTA_ROUTE_REBOOT, "ota_on_reboot", "ota_on_reboot", true},
    {"DELETE", "/file", OTA_ROUTE_DELETE, "ota_on_delete", "ota_on_delete", true},
    {"POST", "/prepare", OTA_ROUTE_PREPARE, "ota_on_prepare", "ota_on_prepare", true},
    {"PUT", "/assets", OTA_ROUTE_ASSETS, "ota_on_assets", "ota_on_piece", true},
    {"GET", "/screenshot", OTA_ROUTE_SCREENSHOT, "ota_on_screenshot", "ota_on_screenshot", true},
    {"GET", "/log", OTA_ROUTE_LOG, "ota_on_log", "ota_on_log", true},
    {"POST", "/console", OTA_ROUTE_CONSOLE, "ota_on_console", "ota_on_console", true},
    {"GET", "/crashes", OTA_ROUTE_CRASHES, "ota_on_crashes", "ota_on_crashes", true},
};
#define N_ROUTES (sizeof(routes) / sizeof(routes[0]))

static char const *const methods[] = {"GET", "PUT", "POST", "DELETE", "PATCH", "HEAD", "OPTIONS", "get"};

static int admit(char const *head, char const *token, ota_request_t *req, char const **why) {
    *why = NULL;
    return ota_request_admit(head, strlen(head), token, req, why);
}

static void head_for(char *out, size_t cap, char const *method, char const *target, char const *auth) {
    snprintf(out, cap, "%s %s HTTP/1.1\r\nHost: badge\r\n%s%s%sContent-Length: 0\r\n\r\n", method, target,
             auth ? "Authorization: " : "", auth ? auth : "", auth ? "\r\n" : "");
}

static bool route_known(char const *method, char const *target) {
    for (size_t i = 0; i < N_ROUTES; i++) {
        if (strcmp(routes[i].method, method) == 0 && strcmp(routes[i].target, target) == 0) {
            return true;
        }
    }
    return false;
}

// Each route: admitted with the token (any case), 401 without it or with another, 400 for a malformed one
static void test_each_route_token(void) {
    char               head[512];
    ota_request_t      req;
    char const        *why;
    for (size_t i = 0; i < N_ROUTES; i++) {
        head_for(head, sizeof(head), routes[i].method, routes[i].target, "Bearer " TOKEN);
        CHECK(admit(head, TOKEN, &req, &why) == 0 && req.route == routes[i].route && why == NULL);

        head_for(head, sizeof(head), routes[i].method, routes[i].target, "bearer 0123456789ABCDEF0123456789ABCDEF");
        CHECK(admit(head, TOKEN, &req, &why) == 0 && req.route == routes[i].route); // the parser lowers both

        head_for(head, sizeof(head), routes[i].method, routes[i].target, NULL);
        CHECK(admit(head, TOKEN, &req, &why) == 401 && why && strcmp(why, "wrong or missing token") == 0);

        head_for(head, sizeof(head), routes[i].method, routes[i].target, "Bearer " OTHER);
        CHECK(admit(head, TOKEN, &req, &why) == 401 && why && strcmp(why, "wrong or missing token") == 0);

        // A refused boot wipes the server's token: nothing matches it, the right token of old included
        head_for(head, sizeof(head), routes[i].method, routes[i].target, "Bearer " TOKEN);
        CHECK(admit(head, "", &req, &why) == 401);

        // Malformed tokens never reach the comparison: the parser refuses them
        static char const *const bad[] = {
            "Bearer 0123456789abcdef0123456789abcde",   // one short
            "Bearer 0123456789abcdef0123456789abcdef0", // one long
            "Bearer 0123456789abcdef0123456789abcdeg",  // not hex
            "Basic " TOKEN,
            TOKEN,
            "Bearer",
            "",
        };
        for (size_t b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
            head_for(head, sizeof(head), routes[i].method, routes[i].target, bad[b]);
            CHECK(admit(head, TOKEN, &req, &why) == 400 && why && strcmp(why, "bad request") == 0);
        }

        // Two tokens, even both right, are a repeated field (#211): refused, not last-wins
        snprintf(head, sizeof(head),
                 "%s %s HTTP/1.1\r\nAuthorization: Bearer " OTHER "\r\nAuthorization: Bearer " TOKEN "\r\n\r\n",
                 routes[i].method, routes[i].target);
        CHECK(admit(head, TOKEN, &req, &why) == 400);
        snprintf(head, sizeof(head),
                 "%s %s HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nauthorization: Bearer " TOKEN "\r\n\r\n",
                 routes[i].method, routes[i].target);
        CHECK(admit(head, TOKEN, &req, &why) == 400);
    }
}

// A known target with another method is 405, an unknown one 404, both "bad request" and both before the token is
// looked at (today's order: the parser answers first)
static void test_wrong_method_and_target(void) {
    char          head[512];
    ota_request_t req;
    char const   *why;
    for (size_t i = 0; i < N_ROUTES; i++) {
        for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
            if (route_known(methods[m], routes[i].target)) {
                continue;
            }
            head_for(head, sizeof(head), methods[m], routes[i].target, "Bearer " TOKEN);
            CHECK(admit(head, TOKEN, &req, &why) == 405 && why && strcmp(why, "bad request") == 0);
            head_for(head, sizeof(head), methods[m], routes[i].target, NULL);
            CHECK(admit(head, TOKEN, &req, &why) == 405);
        }
    }
    static char const *const unknown[] = {
        "/",          "/info/",      "/INFO",        "/infox",      "/info?x=1", "/file/x", "//info",
        "/screenshot?", "/console?a", "info",         "*",           "/log?",     "/log/",   "/crash",
    };
    for (size_t u = 0; u < sizeof(unknown) / sizeof(unknown[0]); u++) {
        for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
            head_for(head, sizeof(head), methods[m], unknown[u], "Bearer " TOKEN);
            int st = admit(head, TOKEN, &req, &why);
            CHECK((st == 404 || st == 400) && why && strcmp(why, "bad request") == 0);
            if (strncmp(unknown[u], "/log?", 5) != 0) {
                CHECK(st == 404);
            }
        }
    }
    // /log takes ?since=<digits> and nothing else
    head_for(head, sizeof(head), "GET", "/log?since=42", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 0 && req.route == OTA_ROUTE_LOG && req.since == 42);
    head_for(head, sizeof(head), "GET", "/log?since=42", NULL);
    CHECK(admit(head, TOKEN, &req, &why) == 401);
    head_for(head, sizeof(head), "GET", "/log?", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 400);
    head_for(head, sizeof(head), "GET", "/log?since=", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 400);
    head_for(head, sizeof(head), "GET", "/log?since=4x", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 400);
    head_for(head, sizeof(head), "GET", "/log?seq=4", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 400);
    head_for(head, sizeof(head), "POST", "/log?since=4", "Bearer " TOKEN);
    CHECK(admit(head, TOKEN, &req, &why) == 405);
}

// Heads that are not a request line plus headers: 400 "bad request" with or without a token
static void test_malformed_heads(void) {
    static char const *const heads[] = {
        "\r\n\r\n",
        "GET\r\n\r\n",
        "GET /info\r\n\r\n",
        "GET /info HTTP/2\r\n\r\n",
        "GET /info HTTP/1.1 extra\r\n\r\n",
        "GET /info FTP/1.1\r\n\r\n",
        "GET /info HTTP/1.1\r\nAuthorization Bearer " TOKEN "\r\n\r\n",
        "GET /info HTTP/1.1\r\nAuthorization : Bearer " TOKEN "\r\n\r\n",
        "GET /info HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\n folded\r\n\r\n",
        "GET /info HTTP/1.1\r\n: Bearer " TOKEN "\r\n\r\n",
        "PUT /firmware HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nContent-Length: -1\r\n\r\n",
        "PUT /firmware HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "PUT /firmware HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nTransfer-Encoding: chunked\r\n\r\n",
        "PUT /firmware HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nX-Content-SHA256: abc\r\n\r\n",
    };
    ota_request_t req;
    char const   *why;
    for (size_t i = 0; i < sizeof(heads) / sizeof(heads[0]); i++) {
        CHECK(admit(heads[i], TOKEN, &req, &why) == 400 && why && strcmp(why, "bad request") == 0);
    }
    // The other statuses the parser gives, answered before the token too
    CHECK(admit("PUT /firmware HTTP/1.1\r\nContent-Encoding: gzip\r\n\r\n", TOKEN, &req, &why) == 415);
    CHECK(admit("PUT /firmware HTTP/1.1\r\nExpect: magic\r\n\r\n", TOKEN, &req, &why) == 417);
    CHECK(strcmp(why, "bad request") == 0);
    // A good one for contrast: the body headers come through
    CHECK(admit("PUT /file HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nX-Path: FLASH0:a.txt\r\nContent-Length: 3\r\n"
                "X-Content-SHA256: " SHA "\r\nExpect: 100-continue\r\n\r\n",
                TOKEN, &req, &why) == 0);
    CHECK(req.route == OTA_ROUTE_FILE && req.content_length == 3 && req.has_sha256 && req.expect_continue);
    CHECK(req.has_path && strcmp(req.path, "FLASH0:a.txt") == 0 && !req.has_offset);
}

static bool wiped(void const *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char const *)buf)[i]) {
            return false;
        }
    }
    return true;
}

// Open a request the way handle_connection does and run what it hands back; ran says which handler that was. The head
// and the request's token copy are wiped once the token is checked, whatever the verdict (#456).
static int open_and_run(char *head, char const *token, bool *up_front, char const **why) {
    ota_request_t req;
    ota_verdict_t v        = {.status = -1};
    ran                    = NULL;
    size_t       head_len  = strlen(head);
    ota_route_fn handler   = ota_route_open(head, head_len, token, &req, &v);
    CHECK(wiped(head, head_len) && wiped(req.token, sizeof(req.token)));
    int          status  = v.status;
    *why                 = v.why;
    *up_front            = v.up_front;
    CHECK((handler == NULL) == (status != 0));
    if (handler) {
        ota_conn_t const conn = {.fd = -1, .req = &req, .pre = NULL, .pre_len = 0};
        CHECK(handler(&conn) == (req.route == OTA_ROUTE_REBOOT));
    }
    return status;
}

// The table (#447): every route the parser gives has exactly the row the old switch had, every row is reached only
// with the token, and no route value outside the table has one
static void test_table(void) {
    char        head[512];
    bool        up_front;
    char const *why;
    bool        covered[OTA_ROUTE_CRASHES + 1] = {false};
    for (size_t i = 0; i < N_ROUTES; i++) {
        CHECK(!covered[routes[i].route]);
        covered[routes[i].route] = true;
        CHECK(ota_route_row(routes[i].route) != NULL && ota_route_row(routes[i].route)->whole != NULL);

        head_for(head, sizeof(head), routes[i].method, routes[i].target, "Bearer " TOKEN);
        CHECK(open_and_run(head, TOKEN, &up_front, &why) == 0);
        CHECK(ran && strcmp(ran, routes[i].whole) == 0 && up_front == routes[i].up_front);

        snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\nX-Offset: 0\r\n\r\n",
                 routes[i].method, routes[i].target);
        CHECK(open_and_run(head, TOKEN, &up_front, &why) == 0);
        CHECK(ran && strcmp(ran, routes[i].piece) == 0 && up_front == routes[i].up_front);

        // Without the token, with another, or with the server's wiped: refused before any handler, piece or not
        char const *const refused[][2] = {
            {NULL, TOKEN}, {"Bearer " OTHER, TOKEN}, {"Bearer " TOKEN, ""}, {"Bearer " OTHER, OTHER "0"}};
        for (size_t r = 0; r < sizeof(refused) / sizeof(refused[0]); r++) {
            head_for(head, sizeof(head), routes[i].method, routes[i].target, refused[r][0]);
            CHECK(open_and_run(head, refused[r][1], &up_front, &why) == 401 && ran == NULL);
            CHECK(why && strcmp(why, "wrong or missing token") == 0);
            snprintf(head, sizeof(head), "%s %s HTTP/1.1\r\n%s%s%sX-Offset: 4096\r\n\r\n", routes[i].method,
                     routes[i].target, refused[r][0] ? "Authorization: " : "", refused[r][0] ? refused[r][0] : "",
                     refused[r][0] ? "\r\n" : "");
            CHECK(open_and_run(head, refused[r][1], &up_front, &why) == 401 && ran == NULL);
        }
        // A wrong method or a malformed head never reaches a handler either
        head_for(head, sizeof(head), "PATCH", routes[i].target, "Bearer " TOKEN);
        CHECK(open_and_run(head, TOKEN, &up_front, &why) == 405 && ran == NULL);
        snprintf(head, sizeof(head), "%s %s\r\nAuthorization: Bearer " TOKEN "\r\n\r\n", routes[i].method,
                 routes[i].target);
        CHECK(open_and_run(head, TOKEN, &up_front, &why) == 400 && ran == NULL);
    }
    for (int r = 0; r <= OTA_ROUTE_CRASHES; r++) {
        CHECK(covered[r]); // every route value is one the parser gives, and tested above
    }
    CHECK(ota_route_row((ota_route_t)(OTA_ROUTE_CRASHES + 1)) == NULL);
    CHECK(ota_route_row((ota_route_t)-1) == NULL);
}

// What an admitted request always is, however its head was mangled
static void admit_mutated(uint8_t const *buf, size_t len) {
    uint8_t *exact = mut_exact(buf, len); // exactly len bytes: ASan sees any read past them
    CHECK(exact != NULL);
    if (!exact) {
        return;
    }
    ota_request_t req;
    char const   *why = NULL;
    int           st  = ota_request_admit((char const *)exact, len, TOKEN, &req, &why);
    CHECK(st == 0 || st == 400 || st == 401 || st == 404 || st == 405 || st == 415 || st == 417);
    if (st == 0) {
        CHECK(why == NULL && req.has_token && strcmp(req.token, TOKEN) == 0);
    } else {
        CHECK(why && strcmp(why, st == 401 ? "wrong or missing token" : "bad request") == 0);
    }
    ota_verdict_t v       = {.status = -1};
    ota_route_fn  handler = ota_route_open((char *)exact, len, TOKEN, &req, &v);
    CHECK(v.status == st && (handler != NULL) == (st == 0));
    CHECK(!handler || req.has_token);
    CHECK(wiped(exact, len) && wiped(req.token, sizeof(req.token))); // #456: checked, then gone, admitted or not
    free(exact);
}

// Seeded mutations of every route's head (truncated, bit-flipped, NUL/CR/LF inserted, lines repeated): no crash, no
// overread, and nothing admitted without the token. OTA_ROUTE_ROUNDS raises the rounds for a long run.
static void test_mutations(void) {
    static uint8_t a[1024], b[1024];
    char const    *env    = getenv("OTA_ROUTE_ROUNDS");
    long           rounds = env ? atol(env) : 2000;
    mut_rng_t      rng;
    mut_seed(&rng, 447);
    for (size_t i = 0; i < N_ROUTES; i++) {
        char head[512];
        head_for(head, sizeof(head), routes[i].method, routes[i].target, "Bearer " TOKEN);
        size_t len = strlen(head);
        for (size_t cut = 0; cut <= len; cut++) {
            admit_mutated((uint8_t const *)head, cut);
        }
        for (long round = 0; round < rounds; round++) {
            size_t n = mut_truncate((uint8_t const *)head, len, len, a, sizeof(a));
            int    k = 1 + (int)mut_below(&rng, 4);
            for (int s = 0; s < k; s++) {
                n = mut_random(&rng, a, n, b, sizeof(b));
                memcpy(a, b, n);
            }
            admit_mutated(a, n);
        }
    }
}

int main(void) {
    test_each_route_token();
    test_wrong_method_and_target();
    test_malformed_heads();
    test_table();
    test_mutations();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_route_test: all passed\n");
    return 0;
}
