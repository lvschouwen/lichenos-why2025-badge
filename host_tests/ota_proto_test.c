// Host tests for the OTA receiver's request parsing, path rules, token check and SHA-256
#include "mutate.h"
#include "ota_proto.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                    \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define TOKEN "0123456789abcdef0123456789abcdef"
#define SHA   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"

static int parse(char const *head, ota_request_t *req) {
    return ota_parse_request(head, strlen(head), req);
}

static void sha_hex(void const *data, size_t len, char *out) {
    sha256_ctx_t ctx;
    uint8_t      digest[SHA256_DIGEST_LEN];
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
    ota_hex(digest, sizeof(digest), out);
}

static void test_sha256(void) {
    char out[OTA_SHA256_HEX + 1];

    sha_hex("", 0, out);
    CHECK(strcmp(out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha_hex("abc", 3, out);
    CHECK(strcmp(out, SHA) == 0);
    char const *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha_hex(two_blocks, strlen(two_blocks), out);
    CHECK(strcmp(out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);

    // Fed in uneven pieces, as the network delivers it
    sha256_ctx_t ctx;
    uint8_t      digest[SHA256_DIGEST_LEN];
    char         chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    sha256_init(&ctx);
    for (int i = 0; i < 1000; i++) {
        sha256_update(&ctx, chunk, 997);
        sha256_update(&ctx, chunk, 3);
    }
    sha256_final(&ctx, digest);
    ota_hex(digest, sizeof(digest), out);
    CHECK(strcmp(out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
}

static void test_head_length(void) {
    char const *head = "GET /info HTTP/1.1\r\nHost: x\r\n\r\nBODY";
    CHECK(ota_head_length(head, strlen(head)) == strlen(head) - 4);
    CHECK(ota_head_length(head, 20) == 0);
}

static void test_parse(void) {
    ota_request_t req;

    CHECK(parse("GET /info HTTP/1.1\r\nHost: badge\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_INFO);
    CHECK(!req.has_token && req.content_length == -1);

    CHECK(parse("PUT /firmware HTTP/1.1\r\n"
                "authorization: Bearer " TOKEN "\r\n"
                "Content-Length: 1781936\r\n"
                "X-Content-SHA256: " "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD" "\r\n"
                "Expect: 100-continue\r\n\r\n",
                &req) == 0);
    CHECK(req.route == OTA_ROUTE_FIRMWARE);
    CHECK(req.has_token && strcmp(req.token, TOKEN) == 0);
    CHECK(req.content_length == 1781936);
    CHECK(req.has_sha256 && strcmp(req.sha256, SHA) == 0);
    CHECK(req.expect_continue);

    CHECK(parse("PUT /file HTTP/1.1\r\nX-Path: FLASH0:[BADGEVMS.APPS.splitflap]splitflap.elf\r\nContent-Length: 0\r\n\r\n",
                &req) == 0);
    CHECK(req.route == OTA_ROUTE_FILE);
    CHECK(req.has_path && strcmp(req.path, "FLASH0:[BADGEVMS.APPS.splitflap]splitflap.elf") == 0);
    CHECK(req.content_length == 0);

    // Compressed bodies: zlib-wrapped deflate, as HTTP's "deflate" means
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Encoding: deflate\r\n\r\n", &req) == 0);
    CHECK(req.deflate);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Encoding: identity\r\n\r\n", &req) == 0);
    CHECK(!req.deflate);
    CHECK(parse("PUT /firmware HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK(!req.deflate);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Encoding: gzip\r\n\r\n", &req) == 415);

    CHECK(parse("DELETE /file HTTP/1.1\r\nX-Path: FLASH0:[BADGEVMS.APPS]old.json\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_DELETE);
    CHECK(req.has_path && strcmp(req.path, "FLASH0:[BADGEVMS.APPS]old.json") == 0);

    // Chunked uploads: one piece of a larger file, which is checked as a whole once complete
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Offset: 131072\r\nX-Total-Size: 1846624\r\n"
                "X-File-SHA256: " SHA "\r\n\r\n",
                &req) == 0);
    CHECK(req.has_offset && req.offset == 131072);
    CHECK(req.total_size == 1846624);
    CHECK(req.has_file_sha256 && strcmp(req.file_sha256, SHA) == 0);
    CHECK(parse("PUT /firmware HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK(!req.has_offset && req.total_size == -1 && !req.has_file_sha256);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Offset: 12x\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Total-Size: -1\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-File-SHA256: abc\r\n\r\n", &req) == 400);

    CHECK(parse("POST /reboot HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_REBOOT);
    CHECK(parse("POST /prepare HTTP/1.1\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_PREPARE);
    CHECK(parse("GET /prepare HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("PUT /assets HTTP/1.1\r\nX-Offset: 0\r\nX-Total-Size: 300\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_ASSETS && req.has_offset && req.total_size == 300);
    CHECK(parse("GET /assets HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("GET /screenshot HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_SCREENSHOT && req.has_token);
    CHECK(parse("PUT /screenshot HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("POST /screenshot HTTP/1.1\r\n\r\n", &req) == 405);

    CHECK(parse("GET /log HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_LOG && req.has_token && req.since == 0);
    CHECK(parse("GET /log?since=1234 HTTP/1.1\r\n\r\n", &req) == 0 && req.route == OTA_ROUTE_LOG && req.since == 1234);
    CHECK(parse("GET /log?since=0 HTTP/1.1\r\n\r\n", &req) == 0 && req.since == 0);
    CHECK(parse("GET /log?since= HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /log?since=-1 HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /log?since=1x HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("GET /log?x=1 HTTP/1.1\r\n\r\n", &req) == 400);
    CHECK(parse("POST /log HTTP/1.1\r\n\r\n", &req) == 405);
    // #170: GET /crashes takes the token like the rest, no query, no other method
    CHECK(parse("GET /crashes HTTP/1.1\r\nAuthorization: Bearer " TOKEN "\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_CRASHES && req.has_token);
    CHECK(parse("POST /crashes HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("DELETE /crashes HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("GET /crashes?x=1 HTTP/1.1\r\n\r\n", &req) == 404);
    CHECK(parse("POST /console HTTP/1.1\r\nContent-Length: 6\r\nAuthorization: Bearer " TOKEN "\r\n\r\n", &req) == 0);
    CHECK(req.route == OTA_ROUTE_CONSOLE && req.content_length == 6 && req.has_token);
    CHECK(parse("GET /console HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("POST /console?x=1 HTTP/1.1\r\n\r\n", &req) == 404);

    // Wrong method, unknown path, junk
    CHECK(parse("GET /firmware HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("PUT /info HTTP/1.1\r\n\r\n", &req) == 405);
    CHECK(parse("GET /nope HTTP/1.1\r\n\r\n", &req) == 404);
    CHECK(parse("GET /info?x=1 HTTP/1.1\r\n\r\n", &req) == 404);
    CHECK(parse("garbage\r\n\r\n", &req) == 400);

    // Malformed values are refused rather than half-read
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Length: 12x\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Length: -5\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nAuthorization: Bearer short\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nAuthorization: Basic " TOKEN "\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Content-SHA256: abc\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", &req) == 400);
    char long_path[400] = "PUT /file HTTP/1.1\r\nX-Path: FLASH0:[A]";
    memset(long_path + strlen(long_path), 'x', 250);
    strcat(long_path, "\r\n\r\n");
    CHECK(parse(long_path, &req) == 400);
}

// Everything a parse decided, as one line: a recorded request must give the same line as before the #211 refactor
static void describe(int status, ota_request_t const *r, char *out, size_t cap) {
    snprintf(out, cap,
             "st=%d route=%d cl=%lld exp=%d defl=%d tok=%d:%s sha=%d:%s path=%d:%s off=%d:%lld total=%lld since=%lld "
             "fsha=%d:%s",
             status, (int)r->route, r->content_length, r->expect_continue, r->deflate, r->has_token, r->token,
             r->has_sha256, r->sha256, r->has_path, r->path, r->has_offset, r->offset, r->total_size, r->since,
             r->has_file_sha256, r->file_sha256);
}

#define CURL_HEAD(method, target) method " " target " HTTP/1.1\r\nHost: badge:8080\r\nUser-Agent: curl/8.5.0\r\n"   \
                                  "Accept: */*\r\nAuthorization: Bearer " TOKEN "\r\n"
#define UPPER_SHA "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"

// Request heads as tools/badge.sh (curl) sends them, each with what the parser made of it before #211
static struct {
    char const *head;
    char const *parsed;
} const recorded[] = {
    {CURL_HEAD("GET", "/info") "\r\n",
     "st=0 route=0 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("PUT", "/firmware") "X-Content-SHA256: " SHA "\r\nContent-Encoding: deflate\r\nContent-Length: 912345\r\n\r\n",
     "st=0 route=1 cl=912345 exp=0 defl=1 tok=1:" TOKEN " sha=1:" SHA " path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("PUT", "/firmware") "X-Content-SHA256: " SHA "\r\nX-Offset: 131072\r\nX-Total-Size: 1784656\r\n"
     "X-File-SHA256: " UPPER_SHA "\r\nX-Pad: xxxxx\r\nContent-Length: 131072\r\n\r\n",
     "st=0 route=1 cl=131072 exp=0 defl=0 tok=1:" TOKEN " sha=1:" SHA " path=0: off=1:131072 total=1784656 since=0 fsha=1:" SHA},
    {CURL_HEAD("PUT", "/file") "X-Content-SHA256: " SHA "\r\nX-Offset: 0\r\nX-Total-Size: 1513520\r\nX-File-SHA256: " SHA
     "\r\nX-Path: SD0:[BADGEVMS.APPS.why2025_firmware_ota_c6]firmware.bin\r\nX-Pad: x\r\nContent-Length: 131072\r\n\r\n",
     "st=0 route=2 cl=131072 exp=0 defl=0 tok=1:" TOKEN " sha=1:" SHA
     " path=1:SD0:[BADGEVMS.APPS.why2025_firmware_ota_c6]firmware.bin off=1:0 total=1513520 since=0 fsha=1:" SHA},
    {CURL_HEAD("PUT", "/assets") "X-Content-SHA256: " SHA "\r\nX-Offset: 262144\r\nX-Total-Size: 300000\r\nX-File-SHA256: "
     SHA "\r\nX-Pad: xxxxxxxxxxxxxxxx\r\nContent-Length: 37856\r\n\r\n",
     "st=0 route=6 cl=37856 exp=0 defl=0 tok=1:" TOKEN " sha=1:" SHA " path=0: off=1:262144 total=300000 since=0 fsha=1:" SHA},
    {CURL_HEAD("PUT", "/file") "X-Content-SHA256: " SHA "\r\nContent-Encoding: deflate\r\nX-Path: FLASH0:[BADGEVMS.APPS]x.json"
     "\r\nContent-Length: 77\r\n\r\n",
     "st=0 route=2 cl=77 exp=0 defl=1 tok=1:" TOKEN " sha=1:" SHA " path=1:FLASH0:[BADGEVMS.APPS]x.json off=0:0 total=-1 "
     "since=0 fsha=0:"},
    {CURL_HEAD("DELETE", "/file") "X-Path: FLASH0:[BADGEVMS.APPS]old.json\r\n\r\n",
     "st=0 route=4 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=1:FLASH0:[BADGEVMS.APPS]old.json off=0:0 total=-1 since=0 "
     "fsha=0:"},
    {CURL_HEAD("POST", "/reboot") "\r\n",
     "st=0 route=3 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("POST", "/prepare") "\r\n",
     "st=0 route=5 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("GET", "/screenshot") "\r\n",
     "st=0 route=7 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("GET", "/log?since=4711") "\r\n",
     "st=0 route=8 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=4711 fsha=0:"},
    {CURL_HEAD("GET", "/crashes") "\r\n",
     "st=0 route=10 cl=-1 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    {CURL_HEAD("POST", "/console") "Content-Type: text/plain\r\nContent-Length: 8\r\n\r\n",
     "st=0 route=9 cl=8 exp=0 defl=0 tok=1:" TOKEN " sha=0: path=0: off=0:0 total=-1 since=0 fsha=0:"},
    // An older client that lets curl send Expect, with a value padded by blanks
    {CURL_HEAD("PUT", "/firmware") "X-Content-SHA256:  " UPPER_SHA " \t\r\nContent-Length: 1781936\r\nExpect: 100-continue\r\n\r\n",
     "st=0 route=1 cl=1781936 exp=1 defl=0 tok=1:" TOKEN " sha=1:" SHA " path=0: off=0:0 total=-1 since=0 fsha=0:"},
    // Refusals a valid client meets: wrong token format, wrong route, wrong method, unsupported encoding
    {CURL_HEAD("GET", "/nope") "\r\n", NULL},
    {CURL_HEAD("PUT", "/info") "\r\n", NULL},
    {CURL_HEAD("PUT", "/firmware") "Content-Encoding: gzip\r\n\r\n", NULL},
};

// #211: the refactored parser gives valid clients exactly what the old one did, field by field
static void test_recorded_valid(void) {
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        ota_request_t req;
        char          got[1024];
        int           st = parse(recorded[i].head, &req);
        describe(st, &req, got, sizeof(got));
        if (recorded[i].parsed) {
            if (strcmp(got, recorded[i].parsed) != 0) {
                printf("recorded %zu:\n  got  %s\n  want %s\n", i, got, recorded[i].parsed);
            }
            CHECK(strcmp(got, recorded[i].parsed) == 0);
        }
    }
    CHECK(parse(recorded[14].head, &(ota_request_t){0}) == 404);
    CHECK(parse(recorded[15].head, &(ota_request_t){0}) == 405);
    CHECK(parse(recorded[16].head, &(ota_request_t){0}) == 415);
}

// #211: a header that counts, sent twice, is refused rather than the last copy silently winning
static void test_repeated_headers(void) {
    static char const *const once[] = {
        "Content-Length: 5",       "X-Offset: 0",           "X-Total-Size: 5",   "X-File-SHA256: " SHA,
        "Authorization: Bearer " TOKEN, "X-Content-SHA256: " SHA, "X-Path: SD0:a.txt", "Content-Encoding: identity",
        "Expect: 100-continue",    "X-Piece-Size: 5",       "X-Reboot: hold",
    };
    for (size_t i = 0; i < sizeof(once) / sizeof(once[0]); i++) {
        char          head[512];
        ota_request_t req;
        snprintf(head, sizeof(head), "PUT /file HTTP/1.1\r\n%s\r\n\r\n", once[i]);
        CHECK(parse(head, &req) == 0);
        snprintf(head, sizeof(head), "PUT /file HTTP/1.1\r\n%s\r\n%s\r\n\r\n", once[i], once[i]);
        if (parse(head, &req) != 400) {
            printf("repeated header accepted: %s\n", once[i]);
        }
        CHECK(parse(head, &req) == 400);
    }
    // Different case, different value: still the same header
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length: 5\r\ncontent-length: 6\r\n\r\n", &(ota_request_t){0}) == 400);
    // Headers the receiver does not read may repeat (proxies and curl add some)
    CHECK(parse("GET /info HTTP/1.1\r\nAccept: */*\r\nAccept: text/plain\r\n\r\n", &(ota_request_t){0}) == 0);
}

// #298: a compressed piece says how many bytes it decodes to; the value follows the size rules of the other headers
static void test_piece_size(void) {
    ota_request_t req;
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Offset: 131072\r\nX-Total-Size: 2630000\r\nContent-Length: 70000\r\n"
                "Content-Encoding: deflate\r\nX-Piece-Size: 131072\r\n\r\n",
                &req) == 0);
    CHECK(req.deflate && req.piece_size == 131072 && req.content_length == 70000 && req.offset == 131072);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Offset: 0\r\n\r\n", &req) == 0 && req.piece_size == -1);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nx-piece-size: 0\r\n\r\n", &req) == 0 && req.piece_size == 0);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Piece-Size: -1\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Piece-Size: 12x\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Piece-Size: \r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Piece-Size: 1000000000000\r\n\r\n", &req) == 400); // 13 digits
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Piece-Size: 999999999999\r\n\r\n", &req) == 0 &&
          req.piece_size == 999999999999LL);
}

// #298: X-Reboot takes one of two words; absent is "auto" (the badge restarts by itself), as an old client gets
static void test_reboot_header(void) {
    ota_request_t req;
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: hold\r\n\r\n", &req) == 0 && req.hold_reboot);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nx-reboot:  HOLD \r\n\r\n", &req) == 0 && req.hold_reboot);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: auto\r\n\r\n", &req) == 0 && !req.hold_reboot);
    CHECK(parse("PUT /firmware HTTP/1.1\r\n\r\n", &req) == 0 && !req.hold_reboot);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: holdx\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: \r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: no\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nX-Reboot: hold\r\nX-Reboot: auto\r\n\r\n", &req) == 400);
    static char const nul[] = "PUT /firmware HTTP/1.1\r\nX-Reboot: ho\0ld\r\n\r\n";
    CHECK(ota_parse_request(nul, sizeof(nul) - 1, &req) == 400);
}

// #211: a NUL inside a line used to cut the value short (X-Path: a.txt\0junk read as a.txt); it is refused now
static void test_embedded_nul(void) {
    static char const head[] = "PUT /file HTTP/1.1\r\nX-Path: SD0:a.txt\0junk\r\n\r\n";
    ota_request_t     req;
    CHECK(ota_parse_request(head, sizeof(head) - 1, &req) == 400);
    static char const in_line[] = "GET /info\0 HTTP/1.1\r\n\r\n";
    CHECK(ota_parse_request(in_line, sizeof(in_line) - 1, &req) == 400);
    static char const in_piece[] = "PUT /firmware HTTP/1.1\r\nX-Piece-Size: 13\0" "1072\r\n\r\n"; // #298
    CHECK(ota_parse_request(in_piece, sizeof(in_piece) - 1, &req) == 400);
    static char const in_name[] = "GET /info HTTP/1.1\r\nAc\0cept: x\r\n\r\n";
    CHECK(ota_parse_request(in_name, sizeof(in_name) - 1, &req) == 400);
}

static bool lower_hex(char const *s, size_t want) {
    if (strlen(s) != want) {
        return false;
    }
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) {
            return false;
        }
    }
    return true;
}

// What any accepted request must look like, however it was mangled
static void check_invariants(int st, ota_request_t const *r) {
    CHECK(st == 0 || st == 400 || st == 404 || st == 405 || st == 415 || st == 417);
    if (st != 0) {
        return;
    }
    CHECK(r->content_length >= -1 && r->total_size >= -1 && r->offset >= 0 && r->since >= 0 && r->piece_size >= -1);
    CHECK(r->piece_size < 1000000000000LL);
    CHECK(r->content_length < 1000000000000LL && r->total_size < 1000000000000LL && r->offset < 1000000000000LL);
    CHECK(!r->has_token || lower_hex(r->token, OTA_TOKEN_LEN));
    CHECK(!r->has_sha256 || lower_hex(r->sha256, OTA_SHA256_HEX));
    CHECK(!r->has_file_sha256 || lower_hex(r->file_sha256, OTA_SHA256_HEX));
    CHECK(memchr(r->path, '\0', sizeof(r->path)) != NULL);
    CHECK(!r->has_path || (strlen(r->path) > 0 && strlen(r->path) <= OTA_PATH_MAX));
}

static void parse_mutated(uint8_t const *buf, size_t len) {
    uint8_t *exact = mut_exact(buf, len); // exactly len bytes: ASan sees any read past them
    CHECK(exact != NULL);
    if (!exact) {
        return;
    }
    ota_request_t req;
    int           st = ota_parse_request((char const *)exact, len, &req);
    check_invariants(st, &req);
    free(exact);
}

// #211: seeded mutations of the recorded requests (truncated, bit-flipped, NUL/CR/LF inserted, lines repeated, length
// fields lying) under ASan+UBSan; never a crash, an overread or an accepted request that breaks the invariants
static void test_mutations(void) {
    enum { ROUNDS = 400, STACK = 4 };
    static uint8_t a[1024], b[1024];
    mut_rng_t      rng;
    mut_seed(&rng, 211);
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        size_t len = strlen(recorded[i].head);
        for (size_t cut = 0; cut <= len; cut++) {
            parse_mutated((uint8_t const *)recorded[i].head, cut);
        }
        static char const *const lies[][2] = {
            {"Content-Length: ", "Content-Length: 999999999999"}, {"Content-Length: ", "Content-Length: 9999999999999"},
            {"X-Offset: ", "X-Offset: -"}, {"X-Total-Size: ", "X-Total-Size: 0x"}, {"Content-Length: ", "Content-Length: +"},
            {"X-Path: ", "X-Path: "}, {"/log?since=", "/log?since=99999999999999999999"},
        };
        for (size_t l = 0; l < sizeof(lies) / sizeof(lies[0]); l++) {
            size_t n = mut_replace((uint8_t const *)recorded[i].head, len, lies[l][0], lies[l][1], a, sizeof(a));
            if (n) {
                parse_mutated(a, n);
            }
        }
        for (int round = 0; round < ROUNDS; round++) {
            size_t n = mut_truncate((uint8_t const *)recorded[i].head, len, len, a, sizeof(a));
            int    k = 1 + (int)mut_below(&rng, STACK);
            for (int s = 0; s < k; s++) {
                n = mut_random(&rng, a, n, b, sizeof(b));
                memcpy(a, b, n);
            }
            parse_mutated(a, n);
        }
    }
    // A head of nothing but one long line, and one past the line limit by one
    memset(a, 'A', sizeof(a));
    parse_mutated(a, sizeof(a));
    memcpy(a + 255, "\r\n\r\n", 4);
    parse_mutated(a, 259);
    memcpy(a + 256, "\r\n\r\n", 4);
    parse_mutated(a, 260);
}

// The helpers themselves (mutate.h is shared with the radio decoder tests)
static void test_mutate_helpers(void) {
    uint8_t     out[32];
    char const *src = "abc:def";
    CHECK(mut_truncate((uint8_t const *)src, 7, 3, out, sizeof(out)) == 3 && memcmp(out, "abc", 3) == 0);
    CHECK(mut_repeat((uint8_t const *)src, 7, 0, 4, out, sizeof(out)) == 11 && memcmp(out, "abc:abc:def", 11) == 0);
    CHECK(mut_repeat((uint8_t const *)src, 7, 0, 4, out, 10) == 0);
    CHECK(mut_insert((uint8_t const *)src, 7, 3, "\0", 1, out, sizeof(out)) == 8 && memcmp(out, "abc\0:def", 8) == 0);
    CHECK(mut_delete((uint8_t const *)src, 7, 3, 1, out, sizeof(out)) == 6 && memcmp(out, "abcdef", 6) == 0);
    CHECK(mut_delete((uint8_t const *)src, 7, 6, 2, out, sizeof(out)) == 0);
    CHECK(mut_replace((uint8_t const *)src, 7, "def", "12345", out, sizeof(out)) == 9 && memcmp(out, "abc:12345", 9) == 0);
    CHECK(mut_replace((uint8_t const *)src, 7, "xyz", "1", out, sizeof(out)) == 0);
    memcpy(out, "abcd", 4);
    mut_flip_bit(out, 4, 8 * 3 + 1);
    CHECK(out[3] == ('d' ^ 2));
    mut_lie_le(out, 4, 2, 4, 0x11223344); // clipped at the end of the buffer
    CHECK(out[2] == 0x44 && out[3] == 0x33);
    mut_rng_t r1, r2;
    mut_seed(&r1, 7);
    mut_seed(&r2, 7);
    CHECK(mut_next(&r1) == mut_next(&r2));
}

static void test_paths(void) {
    CHECK(ota_path_allowed("FLASH0:[BADGEVMS.APPS.splitflap]splitflap.elf"));
    CHECK(ota_path_allowed("FLASH0:[BADGEVMS.APPS]splitflap.json"));
    CHECK(ota_path_allowed("SD0:[BADGEVMS.APPS.why2025_firmware_ota_c6]firmware.bin.md5"));
    CHECK(ota_path_allowed("SD0:notes.txt"));

    CHECK(!ota_path_allowed(""));
    CHECK(!ota_path_allowed("APPS:[x]y"));             // logical names resolve to more than one place
    CHECK(!ota_path_allowed("FLASH1:[A]b"));
    CHECK(!ota_path_allowed("flash0:[A]b"));
    CHECK(!ota_path_allowed("FLASH0:[A]"));            // no file name
    CHECK(!ota_path_allowed("FLASH0:[A..B]c"));        // empty directory part
    CHECK(!ota_path_allowed("FLASH0:[.A]c"));
    CHECK(!ota_path_allowed("FLASH0:[A.]c"));
    CHECK(!ota_path_allowed("FLASH0:[]c"));
    CHECK(!ota_path_allowed("FLASH0:[A]b]c"));
    CHECK(!ota_path_allowed("FLASH0:[A[B]c"));
    CHECK(!ota_path_allowed("FLASH0:[A]../c"));
    CHECK(!ota_path_allowed("FLASH0:[A]b/c"));
    CHECK(!ota_path_allowed("FLASH0:[A]b:c"));
    CHECK(!ota_path_allowed("FLASH0:[A].hidden"));
    CHECK(!ota_path_allowed("FLASH0:[A].."));
    CHECK(!ota_path_allowed("FLASH0:[A]" OTA_TEMP_NAME));
    CHECK(!ota_path_allowed("FLASH0:[A]" OTA_TEMP_NAME ".old"));  // the swap's backup name
    CHECK(!ota_path_allowed("FLASH0:[A]b c"));
}

static void test_temp_path(void) {
    char out[OTA_PATH_MAX + 1];
    CHECK(ota_temp_path("FLASH0:[BADGEVMS.APPS.x]x.elf", out, sizeof(out)));
    CHECK(strcmp(out, "FLASH0:[BADGEVMS.APPS.x]" OTA_TEMP_NAME) == 0);
    CHECK(ota_temp_path("SD0:notes.txt", out, sizeof(out)));
    CHECK(strcmp(out, "SD0:" OTA_TEMP_NAME) == 0);
    CHECK(!ota_temp_path("FLASH0:[BADGEVMS.APPS.x]x.elf", out, 10));
}

static void test_token(void) {
    CHECK(ota_token_equal(TOKEN, TOKEN));
    CHECK(!ota_token_equal(TOKEN, "0123456789abcdef0123456789abcdee"));
    CHECK(!ota_token_equal(TOKEN, ""));
    CHECK(!ota_token_equal("", ""));
}

// #44: no route answers without the token any more, /info included
static void test_routes_need_token(void) {
    ota_route_t const routes[] = {OTA_ROUTE_INFO,  OTA_ROUTE_FIRMWARE,   OTA_ROUTE_FILE, OTA_ROUTE_REBOOT,
                                  OTA_ROUTE_DELETE, OTA_ROUTE_PREPARE,   OTA_ROUTE_ASSETS, OTA_ROUTE_SCREENSHOT,
                                  OTA_ROUTE_LOG,   OTA_ROUTE_CONSOLE, OTA_ROUTE_CRASHES};
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        CHECK(ota_route_needs_token(routes[i]));
    }
}

// #302: the routes that stop the apps (and end in a restart) are the ones refused while the image is on trial, since
// that restart rolls the new image back; everything else is served as usual
static void test_routes_stop_apps(void) {
    CHECK(ota_route_stops_apps(OTA_ROUTE_PREPARE));
    CHECK(ota_route_stops_apps(OTA_ROUTE_FIRMWARE));
    CHECK(ota_route_stops_apps(OTA_ROUTE_ASSETS));
    ota_route_t const others[] = {OTA_ROUTE_INFO, OTA_ROUTE_FILE, OTA_ROUTE_REBOOT, OTA_ROUTE_DELETE,
                                  OTA_ROUTE_SCREENSHOT, OTA_ROUTE_LOG, OTA_ROUTE_CONSOLE, OTA_ROUTE_CRASHES};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(!ota_route_stops_apps(others[i]));
    }
}

// #420: while the console's USB hold is on, prepare and firmware (whole or a piece) are refused before anything stops
// or is erased, and so is a file write or delete in the C6 image folder; assets, other files, reboot and the rest boot
// no other image and are served as before
static void test_routes_usb_held(void) {
    CHECK(ota_route_usb_held(OTA_ROUTE_PREPARE, false) == OTA_HOLD_PREPARE);
    CHECK(ota_route_usb_held(OTA_ROUTE_FIRMWARE, false) == OTA_HOLD_FIRMWARE);
    CHECK(ota_route_usb_held(OTA_ROUTE_FILE, true) == OTA_HOLD_C6);
    CHECK(ota_route_usb_held(OTA_ROUTE_DELETE, true) == OTA_HOLD_C6);
    CHECK(ota_route_usb_held(OTA_ROUTE_FILE, false) == OTA_HOLD_NONE);
    CHECK(ota_route_usb_held(OTA_ROUTE_DELETE, false) == OTA_HOLD_NONE);
    ota_route_t const others[] = {OTA_ROUTE_INFO, OTA_ROUTE_REBOOT, OTA_ROUTE_ASSETS, OTA_ROUTE_SCREENSHOT,
                                  OTA_ROUTE_LOG, OTA_ROUTE_CONSOLE, OTA_ROUTE_CRASHES};
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        CHECK(ota_route_usb_held(others[i], false) == OTA_HOLD_NONE);
        CHECK(ota_route_usb_held(others[i], true) == OTA_HOLD_NONE); // a path on another route means nothing
    }
    CHECK(strcmp(ota_status_reason(409), "Conflict") == 0); // the refusal's status line
    char text[512];
    ota_hold_reply(OTA_HOLD_PREPARE, "the list", text, sizeof(text));
    CHECK(strcmp(text, "update refused (prepare: firmware or assets): the list is on; switch it off at the USB console "
                       "first (fw onair off)") == 0);
    ota_hold_reply(OTA_HOLD_FIRMWARE, "the list", text, sizeof(text));
    CHECK(strncmp(text, "firmware update refused: the list is on; switch it off", 54) == 0);
    ota_hold_reply(OTA_HOLD_C6, "the list", text, sizeof(text));
    CHECK(strncmp(text, "C6 image change refused (", 25) == 0 && strstr(text, "LoRa radio"));
    ota_hold_reply(OTA_HOLD_BOOT, "the list", text, sizeof(text));
    CHECK(strncmp(text, "firmware written but not booted: ", 33) == 0);
    ota_hold_reply(OTA_HOLD_NONE, "the list", text, sizeof(text)); // never asked; still a refusal, never garbage
    CHECK(strncmp(text, "firmware update refused: ", 25) == 0);
    ota_hold_reply((ota_hold_t)99, "the list", text, sizeof(text));
    CHECK(strncmp(text, "firmware update refused: ", 25) == 0);
    ota_hold_reply(OTA_HOLD_C6, "the list", text, 16); // truncated, terminated
    CHECK(strlen(text) == 15);
}

// #116: no token read or saved anywhere this boot means the server does not listen
static void test_token_kept(void) {
    CHECK(!ota_token_kept(false, false, false, false));
    CHECK(ota_token_kept(true, false, false, false));
    CHECK(ota_token_kept(false, true, false, false));
    CHECK(ota_token_kept(false, false, true, false));  // new token, saved to /storage only (NVS down)
    CHECK(ota_token_kept(false, false, false, true));  // new token, saved to settings only (no /storage)
}

// #116: the setting alone used to decide; without a kept token the server must not listen even with it on
static void test_server_listens(void) {
    CHECK(!ota_server_listens(true, false));
    CHECK(ota_server_listens(true, true));
    CHECK(!ota_server_listens(false, true));
    CHECK(!ota_server_listens(false, false));
    CHECK(!ota_server_listens(true, ota_token_kept(false, false, false, false))); // nothing read, nothing saved
}

// #299: a 1.5 MB C6 firmware.bin was refused only after 18 s of writing, with 1 MB free; the receiver now checks first.
// The upload is written next to the old file before the swap, so it needs its own clusters plus one of slack.
static void test_file_fits(void) {
    CHECK(!ota_file_fits(1048576, 1513520, 4096)); // the #299 case
    CHECK(ota_file_fits(2621440, 1513520, 4096));
    // LittleFS (#300): 4088 B of a 4096 B block hold data (the rest is CTZ list pointers), plus 4 blocks of metadata
    CHECK(ota_file_fits(1536000, 1513520, 4096)); // 371 data blocks + 4
    CHECK(!ota_file_fits(1535999, 1513520, 4096));
    CHECK(ota_file_fits(20480, 4088, 4096)); // one full block + 4
    CHECK(!ota_file_fits(20479, 4088, 4096));
    CHECK(!ota_file_fits(20480, 4089, 4096)); // a partial block takes a whole one
    CHECK(!ota_file_fits(20480, 4096, 4096)); // a block's worth of data needs two
    CHECK(ota_file_fits(16384, 0, 4096));     // an empty file still needs the metadata margin
    CHECK(!ota_file_fits(16383, 0, 4096));
    CHECK(!ota_file_fits(0, 0, 4096));
    CHECK(ota_file_fits(2, 1, 0));            // no block size: count bytes
    CHECK(!ota_file_fits(UINT64_MAX, UINT64_MAX, 4096)); // no overflow
}

// #403: the reply reason phrases moved out of ota_server.c unchanged; what a client reads on the status line
static void test_status_reason(void) {
    static struct {
        int         status;
        char const *reason;
    } const want[] = {
        {200, "OK"},          {400, "Bad Request"},         {401, "Unauthorized"},       {403, "Forbidden"},
        {404, "Not Found"},   {405, "Method Not Allowed"},  {409, "Conflict"},           {411, "Length Required"},
        {413, "Content Too Large"}, {422, "Unprocessable Content"}, {503, "Service Unavailable"},
        {507, "Insufficient Storage"}, {415, "Unsupported Media Type"}, {500, "Internal Server Error"}, {0, "Internal Server Error"},
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(strcmp(ota_status_reason(want[i].status), want[i].reason) == 0);
    }
}

// #403: the request line used strtok, whose static state any other strtok user in the same task (or another task)
// overwrote. A caller midway through its own strtok split must find its place unchanged after a parse.
static void test_request_line_reentrant(void) {
    char          outer[] = "one two three";
    ota_request_t req;
    CHECK(strcmp(strtok(outer, " "), "one") == 0);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nContent-Length: 5\r\n\r\n", &req) == 0 && req.route == OTA_ROUTE_FIRMWARE);
    char const *next = strtok(NULL, " ");
    CHECK(next && strcmp(next, "two") == 0);
    // The split itself is unchanged: runs of spaces count as one, a fourth word or a missing one is refused
    CHECK(parse("GET  /info   HTTP/1.1\r\n\r\n", &req) == 0 && req.route == OTA_ROUTE_INFO);
    CHECK(parse(" GET /info HTTP/1.1 \r\n\r\n", &req) == 0 && req.route == OTA_ROUTE_INFO);
    CHECK(parse("GET /info HTTP/1.1 x\r\n\r\n", &req) == 400);
    CHECK(parse("GET /info\r\n\r\n", &req) == 400);
    CHECK(parse("   \r\n\r\n", &req) == 400);
    CHECK(parse("GET /log?since=7 HTTP/1.0\r\n\r\n", &req) == 0 && req.route == OTA_ROUTE_LOG && req.since == 7);
}

// #403: "Name : value" used to be read as a header called "Name " and ignored, so "Content-Length : 5" went unseen by
// the receiver while a proxy might read it; HTTP/1.1 says to refuse it. A blank before the colon of any header line of
// every recorded valid request is refused with 400.
static void test_blank_before_colon(void) {
    ota_request_t req;
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length : 5\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length\t: 5\r\n\r\n", &req) == 400);
    CHECK(parse("GET /info HTTP/1.1\r\nAccept : */*\r\n\r\n", &req) == 400); // headers it does not read as well
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length:5\r\n\r\n", &req) == 0 && req.content_length == 5);
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length: \t5\t \r\n\r\n", &req) == 0 && req.content_length == 5);
    static uint8_t out[1024];
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        if (!recorded[i].parsed) {
            continue; // refused for its route or encoding before or regardless of the headers
        }
        char const *head = recorded[i].head;
        size_t      len  = strlen(head);
        size_t      line = (size_t)(strstr(head, "\r\n") - head) + 2; // past the request line
        while (line < len) {
            char const *colon = memchr(head + line, ':', len - line);
            char const *eol   = strstr(head + line, "\r\n");
            if (colon && eol && colon < eol) {
                for (char const *blank = " \t"; *blank; blank++) {
                    size_t n = mut_insert((uint8_t const *)head, len, (size_t)(colon - head), blank, 1, out, sizeof(out));
                    CHECK(n && ota_parse_request((char const *)out, n, &req) == 400);
                }
            }
            line = eol ? (size_t)(eol - head) + 2 : len;
        }
    }
}

// #417: a header line starting with a blank (obsolete folding) used to be read as an unknown header and ignored
static void test_folded_header(void) {
    ota_request_t req;
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length: 5\r\n 6\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length: 5\r\n\t6\r\n\r\n", &req) == 400);
    CHECK(parse("GET /info HTTP/1.1\r\n X: y\r\n\r\n", &req) == 400);
    CHECK(parse("PUT /file HTTP/1.1\r\nContent-Length: 5\r\n\r\n", &req) == 0 && req.content_length == 5);
    static uint8_t out[1024];
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        if (!recorded[i].parsed) {
            continue;
        }
        char const *head = recorded[i].head;
        size_t      len  = strlen(head);
        size_t      line = (size_t)(strstr(head, "\r\n") - head) + 2; // past the request line
        while (line < len && strncmp(head + line, "\r\n", 2) != 0) {
            for (char const *blank = " \t"; *blank; blank++) {
                size_t n = mut_insert((uint8_t const *)head, len, line, blank, 1, out, sizeof(out));
                CHECK(n && ota_parse_request((char const *)out, n, &req) == 400);
            }
            char const *eol = strstr(head + line, "\r\n");
            line            = eol ? (size_t)(eol - head) + 2 : len;
        }
    }
}

// #403: an Expect other than 100-continue used to be ignored; the receiver cannot meet it, so it is refused with 417
static void test_expect(void) {
    ota_request_t req;
    CHECK(parse("PUT /firmware HTTP/1.1\r\nExpect: 100-continue\r\n\r\n", &req) == 0 && req.expect_continue);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nexpect:  100-Continue \r\n\r\n", &req) == 0 && req.expect_continue);
    CHECK(parse("PUT /firmware HTTP/1.1\r\n\r\n", &req) == 0 && !req.expect_continue);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nExpect: 200-ok\r\n\r\n", &req) == 417);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nExpect: \r\n\r\n", &req) == 417);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nExpect: 100-continue, x\r\n\r\n", &req) == 417);
    CHECK(parse("PUT /firmware HTTP/1.1\r\nExpect: 100-continuex\r\n\r\n", &req) == 417);
    static char const nul[] = "PUT /firmware HTTP/1.1\r\nExpect: 100-cont\0inue\r\n\r\n";
    CHECK(ota_parse_request(nul, sizeof(nul) - 1, &req) == 400);
    CHECK(strcmp(ota_status_reason(417), "Expectation Failed") == 0);
    // The recorded client that sends Expect, with its value changed
    static char const *const others[] = {"100-continu", "1OO-continue", "100-continue;q=1", "*", "x"};
    static uint8_t           out[1024];
    for (size_t i = 0; i < sizeof(recorded) / sizeof(recorded[0]); i++) {
        size_t len = strlen(recorded[i].head);
        for (size_t o = 0; o < sizeof(others) / sizeof(others[0]); o++) {
            size_t n = mut_replace((uint8_t const *)recorded[i].head, len, "100-continue", others[o], out, sizeof(out));
            if (n) {
                CHECK(ota_parse_request((char const *)out, n, &req) == 417);
            }
        }
    }
}

// The size field of the partition table line of name (one of the app slots), or 0 when it is not there
static unsigned long slot_size(char const *csv, char const *name) {
    size_t      nl = strlen(name);
    char const *p  = csv;
    for (; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (strncmp(p, name, nl) == 0 && p[nl] == ',') {
            char const *f = p;
            for (int field = 0; field < 4 && f; field++) {
                f = strchr(f, ',');
                f = f ? f + 1 : NULL;
            }
            return f ? strtoul(f, NULL, 0) : 0;
        }
    }
    return 0;
}

static bool slots_match_limit(char const *csv) {
    return slot_size(csv, "ota_0") == OTA_FIRMWARE_MAX && slot_size(csv, "ota_1") == OTA_FIRMWARE_MAX;
}

// #403: stream_erase clamps an erase to the slot, safe only while the firmware limit is the slot size. A documentation
// test only: it re-reads the table with its own parser and passed before #403 too. The real guard is the build: the ota
// component's CMakeLists (both slots one size) and the _Static_assert in ota_server.c (FIRMWARE_MAX == that size).
static void test_slot_size(void) {
    static char csv[4096];
    FILE       *f = fopen(OTA_PARTITIONS_CSV, "r");
    CHECK(f != NULL);
    if (!f) {
        return;
    }
    size_t n = fread(csv, 1, sizeof(csv) - 1, f);
    fclose(f);
    csv[n] = '\0';
    CHECK(slots_match_limit(csv));
    // A table whose second slot shrank (or grew) no longer matches
    static uint8_t out[4096];
    size_t         m = mut_replace((uint8_t const *)csv, n, "0x420000, 0x400000", "0x420000, 0x3F0000", out, sizeof(out) - 1);
    CHECK(m != 0);
    out[m] = '\0';
    CHECK(!slots_match_limit((char const *)out));
}

int main(void) {
    test_file_fits();
    test_token_kept();
    test_server_listens();
    test_routes_need_token();
    test_routes_stop_apps();
    test_routes_usb_held();
    test_sha256();
    test_head_length();
    test_parse();
    test_paths();
    test_temp_path();
    test_token();
    test_recorded_valid();
    test_repeated_headers();
    test_piece_size();
    test_reboot_header();
    test_embedded_nul();
    test_mutate_helpers();
    test_mutations();
    test_status_reason();
    test_request_line_reentrant();
    test_blank_before_colon();
    test_folded_header();
    test_expect();
    test_slot_size();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All ota_proto tests passed\n");
    return 0;
}
