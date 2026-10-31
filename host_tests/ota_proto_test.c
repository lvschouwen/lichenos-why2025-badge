// Host tests for the OTA receiver's request parsing, path rules, token check and SHA-256
#include "ota_proto.h"
#include "sha256.h"

#include <stdio.h>
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

int main(void) {
    test_sha256();
    test_head_length();
    test_parse();
    test_paths();
    test_temp_path();
    test_token();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("All ota_proto tests passed\n");
    return 0;
}
