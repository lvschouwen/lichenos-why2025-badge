// Pure logic of the OTA receiver: the HTTP/1.1 subset it speaks, which badge paths it may write and the token check.
//
//   GET  /info                         running firmware version (no token needed)
//   PUT  /firmware                     body = badgevms.bin, installed into the next OTA slot
//   PUT  /file      X-Path: <VMS path> body = the file, written under FLASH0: or SD0:
//   DELETE /file   X-Path: <VMS path> remove a file (same path rules as PUT)
//   GET  /screenshot                   the screen as a 16-bit BMP (ota_bmp.h, #30); the token is required
//   GET  /log[?since=<seq>]            the badge's log lines from sequence number <seq> on (log_ring.h, #38); the token
//                                      is required. Reply headers: X-Next-Seq (ask for this next), X-Log-Missed
//   GET  /crashes                      the stored crash records as text (#170): reason, task, pc, backtrace, firmware version;
//                                      never RAM dump bytes. The token is required
//   POST /console                      body = one fw command line; the reply is its output (#38); the token is required
//   POST /reboot
//   POST /prepare                      lean firmware: stop everything else for a firmware update, reply "ready" (#76)
//   PUT  /assets                       body = assets.bin (tools/pack_assets.py), in pieces only; used after a reboot (#22)
//
// Every request but /info carries "Authorization: Bearer <token>"; uploads carry "X-Content-SHA256: <hex>" of the
// file as it ends up on the badge. "Content-Encoding: deflate" (a zlib stream) sends it compressed.
// Large uploads can come in pieces: each carries "X-Offset", "X-Total-Size" and "X-File-SHA256" (the whole file);
// its own X-Content-SHA256 covers just that piece, so a damaged piece is resent alone. A piece may be compressed on its
// own (#298): "Content-Encoding: deflate", Content-Length counts the zlib stream and "X-Piece-Size" the bytes it decodes
// to; X-Offset, X-Total-Size and both hashes stay in decoded bytes. The first firmware piece is never compressed.
// "X-Reboot: hold" on the request that completes a firmware or assets install keeps the badge on the running image
// until the sender reboots it (tools/badge.sh --no-reboot, #298); "auto" or no header: it restarts by itself.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_PORT         8080
#define OTA_TOKEN_LEN    32 // hex characters
#define OTA_PATH_MAX     192
#define OTA_SHA256_HEX   64
#define OTA_TEMP_NAME    "ota_upload.tmp"

typedef enum {
    OTA_ROUTE_INFO,
    OTA_ROUTE_FIRMWARE,
    OTA_ROUTE_FILE,
    OTA_ROUTE_REBOOT,
    OTA_ROUTE_DELETE,
    OTA_ROUTE_PREPARE,
    OTA_ROUTE_ASSETS,
    OTA_ROUTE_SCREENSHOT,
    OTA_ROUTE_LOG,
    OTA_ROUTE_CONSOLE,
    OTA_ROUTE_CRASHES,
} ota_route_t;

// Every route needs the token since #44, /info included (it names the firmware version)
bool ota_route_needs_token(ota_route_t route);
// The routes that stop the other apps (and end in a restart, #76): prepare, firmware, assets. While the running image is
// on trial they are refused before anything stops, since that restart would roll the new image back (#302)
bool ota_route_stops_apps(ota_route_t route);

typedef struct {
    ota_route_t route;
    long long   content_length; // -1 when absent
    bool        expect_continue;
    bool        deflate; // body is a zlib stream
    bool        has_token;
    char        token[OTA_TOKEN_LEN + 1];
    bool        has_sha256;
    char        sha256[OTA_SHA256_HEX + 1]; // lowercase
    bool        has_path;
    char        path[OTA_PATH_MAX + 1];
    bool        has_offset;
    long long   offset;
    long long   total_size; // -1 when absent
    long long   piece_size; // X-Piece-Size: decoded bytes of a compressed piece; -1 when absent
    bool        hold_reboot; // X-Reboot: hold
    long long   since; // GET /log: ?since=; 0 when absent
    bool        has_file_sha256;
    char        file_sha256[OTA_SHA256_HEX + 1]; // lowercase
} ota_request_t;

// Length of the request head including the blank line, or 0 while it isn't complete yet
size_t ota_head_length(char const *buf, size_t len);

// Parse a complete request head. Returns 0, or the HTTP status to answer with (400, 404, 405, 415).
int ota_parse_request(char const *head, size_t len, ota_request_t *req);

// Whether a VMS-style path may be written: FLASH0: or SD0:, an optional [dir.sub] and a plain file name
bool ota_path_allowed(char const *path);

// The temporary file next to path that an upload is written to before it replaces path; names starting with
// OTA_TEMP_NAME are reserved for it and its backup
bool ota_temp_path(char const *path, char *out, size_t cap);

// Compare two tokens without leaking how much of them matched
bool ota_token_equal(char const *a, char const *b);

// Whether the update server may listen this boot (#116): only with a token the next boot has too, read from
// /storage or settings or saved to one of them. A token kept nowhere (no /storage and NVS down, e.g. a burned key whose
// partition could not be opened) is one no tool knows and the next boot replaces: Wi-Fi updates are refused instead.
bool ota_token_kept(bool from_file, bool from_nvs, bool wrote_file, bool wrote_nvs);
// The server listens (and Settings shows it on) only with the setting on AND a kept token. Before #116 the setting
// alone decided, and a badge without a kept token listened behind a token no tool knew.
bool ota_server_listens(bool setting_on, bool token_kept);

// Whether a file of size bytes fits on the LittleFS drive (blocks of block bytes) with free_bytes free (#299, #300):
// an upload is written next to the old file before the swap, so it needs all its own blocks (block - 8 bytes of data
// each, the rest is the CTZ list's pointers) plus 4 blocks of metadata. block 0: plain bytes plus one.
bool ota_file_fits(uint64_t free_bytes, uint64_t size, uint32_t block);

// Lowercase hex of bytes; out holds 2 * len + 1 characters
void ota_hex(uint8_t const *bytes, size_t len, char *out);
