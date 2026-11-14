#pragma once
// What the update server's files share (#447): ota_server.c (listener, dispatch, settings, hooks), ota_http.c (replies
// and request bodies), ota_upload.c (whole uploads and deletes), ota_piece_http.c (uploads in pieces), ota_extra.c
// (info, screenshot, log, crashes, console) and ota_token.c. All of it runs on the one server task, one request at a
// time, except the screenshot sender (ota_extra.c). Not for use outside this component.

#include "ota_proto.h"
#include "ota_route.h"
#include "ota_server.h"

#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIRMWARE_MAX    OTA_FIRMWARE_MAX
#define FILE_MAX        (4 * 1024 * 1024) // received into PSRAM before it touches storage (#40)
#define ASSETS_MAX      (3 * 1024 * 1024) // the assets partition (firmware/partitions.csv, #22)
#define READ_TIMEOUT_S  10
#define HEAD_DEADLINE_S 15

// The valid mark also waits for a live PSRAM trial (60 s), so "45 s" (OTA_VALID_AFTER_S) undersells it (#302)
#define TRIAL_REFUSAL                                                                                                  \
    "the running image is still on trial: try again once 'fw slot' shows it valid (about a minute after boot); "        \
    "nothing was stopped"

// ota_server.c
extern ota_hooks_t ota_hooks;
extern bool        ota_firmware_installed;
extern bool        ota_fd_detached; // set by the handler that took the connection over: serve() must not close it

static inline void hook_begin(ota_kind_t kind, char const *name, long long total) {
    if (ota_hooks.begin) {
        ota_hooks.begin(kind, name, total > 0 ? (uint32_t)total : 0);
    }
}

static inline void hook_progress(long long done) {
    if (ota_hooks.progress) {
        ota_hooks.progress((uint32_t)done);
    }
}

static inline bool file_allowed(void) {
    return !ota_hooks.file_ok || ota_hooks.file_ok();
}

static inline void hook_end(bool ok, bool rebooting) {
    if (ota_hooks.end) {
        ota_hooks.end(ok, rebooting);
    }
}

// ota_http.c
// PSRAM (#107): sockets copy, no DMA. Flash is never read into it: esp_flash_read would malloc an internal temp buffer
// as big as the read for it; slot and partition reads go through a FLASH_CHUNK_BOUNCE stack buffer (#364).
#define OTA_BODY_BUF_LEN 4096
extern uint8_t   ota_body_buf[OTA_BODY_BUF_LEN];
extern uint32_t  ota_progress_base; // bytes of earlier pieces, so a piece's progress counts from the start of the file
extern long long ota_last_received; // body bytes of the last request, for error replies

long ota_now_s(void);
bool ota_send_all(int fd, void const *data, size_t len);
void ota_respond(int fd, int status, char const *fmt, ...) __attribute__((format(printf, 3, 4)));
void ota_set_read_timeout(int fd, int seconds, int usec);
void ota_drain_body(int fd, size_t pre_len, long long content_length);
void ota_close_lingering(int fd);

typedef bool (*sink_fn)(void *ctx, uint8_t const *data, size_t len);

// A request body: the bytes already read behind the head, how many come in all (on the wire), whether they are a zlib
// stream, the most decoded bytes allowed, and where they go
typedef struct {
    uint8_t const *pre;
    size_t         pre_len;
    long long      total;
    bool           deflate;
    long long      max;
    sink_fn        sink;
    void          *ctx;
    bool           piece; // progress counts decoded bytes (a piece's place in the file), else bytes on the wire
} body_t;

bool ota_receive_body(int fd, body_t b, char sha_hex[OTA_SHA256_HEX + 1]);

// ota_upload.c
char const *ota_display_name(char const *vms);
bool        ota_is_directory(char const *posix_path);
bool        ota_storage_room(int fd, uint64_t size, size_t pre_len, long long body);
bool        ota_finish_file(int fd, char const *vms, uint8_t const *data, size_t len, long long sent);
bool        ota_finish_assets(int fd, uint8_t const *data, size_t len, long long sent);
bool ota_install_slot(int fd, esp_partition_t const *slot, esp_ota_handle_t handle, size_t len, long long sent);
void ota_respond_begin_failed(int fd, esp_err_t err);

// ota_piece_http.c
bool ota_piece_active(void); // an upload in pieces is under way
void ota_piece_expire(bool now);
bool ota_piece_staged_is(ota_request_t const *req, bool c6, char const *posix);
