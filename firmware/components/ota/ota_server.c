// Wi-Fi update server of the lean firmware (#61), adapted from BadgeVMS's OTA receiver app
// (sdk_apps/ota_receiver/main.c): same protocol (ota_proto.h) so tools/badge.sh ota-* works unchanged. It runs
// whenever Wi-Fi is up; the token guards everything but /info. Bodies are received into memory and checked before
// anything touches flash: flash writes while the network is receiving corrupted the stream on this badge (#40).
#include "ota_server.h"

#include "bootloader_random.h"
#include "c6link.h"
#include "fw_console.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "assets.h"
#include "assets_fmt.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_bmp.h"
#include "ota_path.h"
#include "ota_proto.h"
#include "miniz.h" // the ROM inflater
#include "sha256.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TOKEN_FILE      "/storage/ota_token.txt"
#define FIRMWARE_MAX    (4 * 1024 * 1024) // one app slot (firmware/partitions.csv)
#define FILE_MAX        (4 * 1024 * 1024) // received into PSRAM before it touches storage (#40)
#define ASSETS_MAX      (3 * 1024 * 1024) // the assets partition (firmware/partitions.csv, #22)
#define READ_TIMEOUT_S  10
#define HEAD_DEADLINE_S 15
#define BODY_MIN_RATE   (16 * 1024) // bytes/s a body must average, on top of BODY_GRACE_S
#define BODY_GRACE_S    30
#define SHOT_SLICE      (16 * 1024) // bytes per write of a screenshot, straight from its PSRAM buffer
#define SHOT_SEND_S     10
#define SHOT_TOTAL_S    45 // whole send: a client that drips bytes must not hold the one server task and the buffer
// Flash writes happen on this task, so its stack must be in internal RAM; peaks measured in BadgeVMS: 7 KB
#define SERVER_STACK    16384

static char const TAG[] = "ota";

static char token[OTA_TOKEN_LEN + 1];

// One request at a time
static char    head_buf[2048];
static uint8_t body_buf[4096];

static ota_hooks_t hooks;
static uint32_t    progress_base; // bytes of earlier pieces, so a piece's progress counts from the start of the file
static bool        firmware_installed;

void ota_server_set_hooks(ota_hooks_t const *h) {
    hooks = *h;
}

static void hook_begin(ota_kind_t kind, char const *name, long long total) {
    if (hooks.begin) {
        hooks.begin(kind, name, total > 0 ? (uint32_t)total : 0);
    }
}

static void hook_progress(long long done) {
    if (hooks.progress) {
        hooks.progress((uint32_t)done);
    }
}

static bool file_allowed(void) {
    return !hooks.file_ok || hooks.file_ok();
}

static void hook_end(bool ok, bool rebooting) {
    if (hooks.end) {
        hooks.end(ok, rebooting);
    }
}

// The file's name as the screen shows it: what follows the last ':' or '/'
static char const *display_name(char const *vms) {
    char const *name = vms;
    for (char const *p = vms; *p; p++) {
        if (*p == ':' || *p == '/') {
            name = p + 1;
        }
    }
    return *name ? name : vms;
}

static long long last_received; // body bytes of the last request, for error replies
static long long last_decoded;  // file bytes they decoded to

static long now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static bool send_all(int fd, void const *data, size_t len) {
    char const *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            return false;
        }
        p   += n;
        len -= (size_t)n;
    }
    return true;
}

static void respond(int fd, int status, char const *fmt, ...) __attribute__((format(printf, 3, 4)));

static void respond(int fd, int status, char const *fmt, ...) {
    char    body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    strcat(body, "\n");

    char const *reason;
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 403: reason = "Forbidden"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 409: reason = "Conflict"; break;
        case 411: reason = "Length Required"; break;
        case 413: reason = "Content Too Large"; break;
        case 422: reason = "Unprocessable Content"; break;
        case 503: reason = "Service Unavailable"; break;
        default: reason = "Internal Server Error"; break;
    }
    char head[192];
    int  n = snprintf(
        head,
        sizeof(head),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
        status,
        reason,
        (unsigned)strlen(body)
    );
    if (send_all(fd, head, (size_t)n)) {
        send_all(fd, body, strlen(body));
    }
}

static void set_read_timeout(int fd, int seconds, int usec) {
    struct timeval tv = {.tv_sec = seconds, .tv_usec = usec};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

typedef bool (*sink_fn)(void *ctx, uint8_t const *data, size_t len);

// Where decoded bytes go: hashed, size-capped, then handed to the request's sink
typedef struct {
    sha256_ctx_t sha;
    sink_fn      sink;
    void        *ctx;
    long long    max;
} output_t;

static bool output_bytes(output_t *out, uint8_t const *data, size_t len) {
    if (last_decoded + (long long)len > out->max) {
        return false;
    }
    sha256_update(&out->sha, data, len);
    last_decoded += (long long)len;
    return out->sink(out->ctx, data, len);
}

// Streaming inflate of a zlib body with the ROM's tinfl; its 32 KiB dictionary doubles as the output window
static tinfl_decompressor inflater;
static uint8_t            inflate_window[TINFL_LZ_DICT_SIZE];
static size_t             inflate_ofs;
static bool               inflate_done;

static bool inflate_bytes(output_t *out, uint8_t const *in, size_t len) {
    for (;;) {
        size_t       in_bytes  = len;
        size_t       out_bytes = sizeof(inflate_window) - inflate_ofs;
        tinfl_status status    = tinfl_decompress(&inflater, in, &in_bytes, inflate_window, inflate_window + inflate_ofs,
                                                  &out_bytes, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        in  += in_bytes;
        len -= in_bytes;
        if (out_bytes && !output_bytes(out, inflate_window + inflate_ofs, out_bytes)) {
            return false;
        }
        inflate_ofs = (inflate_ofs + out_bytes) & (sizeof(inflate_window) - 1);
        if (status < TINFL_STATUS_DONE) {
            return false;
        }
        if (status == TINFL_STATUS_DONE) {
            inflate_done = true;
            return len == 0; // nothing may follow the stream
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && len == 0) {
            return true;
        }
    }
}

// Read the body (part of which may already sit behind the head), decode it if it is compressed, hash it and feed it to
// sink. max caps the decoded size. sha_hex is the SHA-256 of the decoded bytes, i.e. of the file as it ends up.
static bool receive_body(
    int            fd,
    uint8_t const *pre,
    size_t         pre_len,
    long long      total,
    bool           deflate,
    long long      max,
    sink_fn        sink,
    void          *ctx,
    char           sha_hex[OTA_SHA256_HEX + 1]
) {
    static output_t out; // static, not on the stack: one transfer at a time
    sha256_init(&out.sha);
    out.sink = sink;
    out.ctx  = ctx;
    out.max  = max;
    if (deflate) {
        tinfl_init(&inflater);
        inflate_ofs  = 0;
        inflate_done = false;
    }

    // A per-read timeout alone lets a client trickle bytes forever; the whole body gets a deadline too
    long      deadline = now_s() + BODY_GRACE_S + (long)(total / BODY_MIN_RATE);
    long long got      = 0;
    last_received      = 0;
    last_decoded       = 0;
    hook_progress(progress_base);
    if (pre_len > (size_t)total) {
        pre_len = (size_t)total;
    }
    while (got < total || pre_len > 0) {
        uint8_t const *chunk;
        size_t         n;
        if (pre_len > 0) {
            chunk   = pre;
            n       = pre_len;
            pre_len = 0;
        } else {
            if (now_s() > deadline) {
                return false;
            }
            size_t  want = total - got < (long long)sizeof(body_buf) ? (size_t)(total - got) : sizeof(body_buf);
            ssize_t r    = read(fd, body_buf, want);
            if (r <= 0) {
                return false;
            }
            chunk = body_buf;
            n     = (size_t)r;
        }
        if (!(deflate ? inflate_bytes(&out, chunk, n) : output_bytes(&out, chunk, n))) {
            return false;
        }
        got           += (long long)n;
        last_received  = got;
        hook_progress(progress_base + got);
    }
    if (deflate && !inflate_done) {
        return false; // the stream ended early
    }

    uint8_t digest[SHA256_DIGEST_LEN];
    sha256_final(&out.sha, digest);
    ota_hex(digest, sizeof(digest), sha_hex);
    return true;
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} memory_ctx_t;

static bool memory_sink(void *ctx, uint8_t const *data, size_t len) {
    memory_ctx_t *mem = ctx;
    if (mem->len + len > mem->cap) {
        // Only compressed bodies grow past their first guess; receive_body caps the total
        size_t   cap   = mem->cap * 2 > mem->len + len ? mem->cap * 2 : mem->len + len;
        uint8_t *data2 = realloc(mem->data, cap);
        if (!data2) {
            return false;
        }
        mem->data = data2;
        mem->cap  = cap;
    }
    memcpy(mem->data + mem->len, data, len);
    mem->len += len;
    return true;
}

// Create every directory leading up to the file at path (a /storage/... path)
static void mkdir_parents(char const *path) {
    char dir[OTA_PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%s", path);
    // Start below the mount point: mkdir on /storage itself fails with EINVAL
    for (char *p = dir + strlen("/storage/"); *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "mkdir %s: %s", dir, strerror(errno));
            }
            *p = '/';
        }
    }
}

static bool write_file(char const *path, uint8_t const *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

// Put a checked file in place: write it next to the target, then swap it in. vms is the path as badge.sh names it.
static bool finish_file(int fd, char const *vms, uint8_t const *data, size_t len, long long sent) {
    char path[OTA_PATH_MAX + 16];
    char temp[sizeof(path) + sizeof(OTA_TEMP_NAME)];
    if (!ota_path_to_posix(vms, path, sizeof(path)) || !ota_temp_to_posix(vms, temp, sizeof(temp))) {
        respond(fd, 400, "bad path");
        return false;
    }
    mkdir_parents(path);
    if (!write_file(temp, data, len)) {
        unlink(temp);
        respond(fd, 500, "couldn't write %s, file left as it was", temp);
        return false;
    }
    // rename doesn't replace on FAT: move the old file aside first and put it back if the swap fails
    char backup[sizeof(temp) + 4];
    snprintf(backup, sizeof(backup), "%s.old", temp);
    unlink(backup);
    bool had_old = rename(path, backup) == 0;
    if (rename(temp, path) != 0) {
        if (had_old) {
            rename(backup, path);
        }
        unlink(temp);
        respond(fd, 500, "couldn't move the upload into place, file left as it was");
        return false;
    }
    if (had_old) {
        unlink(backup);
    }
    ESP_LOGW(TAG, "wrote %s (%zu bytes)", path, len);
    respond(fd, 200, "wrote %zu bytes (%lld sent) to %s", len, sent, vms);
    return true;
}

// Install a checked firmware image into the other app slot; it runs after a reboot, on trial until it proves itself
static bool finish_firmware(int fd, uint8_t const *data, size_t len, long long sent) {
    esp_partition_t const *slot = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t       handle;
    if (!slot || esp_ota_begin(slot, len, &handle) != ESP_OK) {
        respond(fd, 503, "couldn't start an OTA session");
        return false;
    }
    if (esp_ota_write(handle, data, len) != ESP_OK) {
        esp_ota_abort(handle);
        respond(fd, 500, "flash write failed, nothing installed");
        return false;
    }
    esp_err_t err = esp_ota_end(handle); // checks the image: chip, segments, hash
    if (err != ESP_OK) {
        respond(fd, 422, "image failed validation (%s), nothing installed", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_ota_set_boot_partition(slot)) != ESP_OK) {
        respond(fd, 500, "couldn't select %s (%s)", slot->label, esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "firmware installed in %s (%zu bytes)", slot->label, len);
    respond(fd, 200, "installed %zu bytes (%lld sent), runs after a reboot", len, sent);
    firmware_installed = true;
    return true;
}

// Write a checked assets image (#22) over the assets partition. The image is fully checked (header, table, CRC)
// before anything is erased, and read back afterwards. The partition is memory-mapped, not copied: while it is
// rewritten, anything reading it sees erased or half-written bytes. So tools/badge.sh asks for /prepare first (the
// apps and screens stop) and reboots afterwards; consumers copy what they keep (assets.h).
static bool finish_assets(int fd, uint8_t const *data, size_t len, long long sent) {
    assets_view_t view;
    assets_err_t  res = assets_open(data, len, true, &view);
    if (res != ASSETS_OK) {
        respond(fd, 422, "not a usable assets image (%s), nothing written", assets_err_str(res));
        return false;
    }
    esp_partition_t const *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASSETS_PARTITION);
    if (!part || len > part->size) {
        respond(fd, 413, "no assets partition, or the image is larger than it");
        return false;
    }
    size_t    erase = (len + part->erase_size - 1) / part->erase_size * part->erase_size;
    esp_err_t err   = esp_partition_erase_range(part, 0, erase);
    if (err == ESP_OK) {
        err = esp_partition_write(part, 0, data, len);
    }
    for (size_t at = 0; err == ESP_OK && at < len; at += sizeof(body_buf)) {
        size_t n = len - at < sizeof(body_buf) ? len - at : sizeof(body_buf);
        err      = esp_partition_read(part, at, body_buf, n);
        if (err == ESP_OK && memcmp(body_buf, data + at, n) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err != ESP_OK) {
        respond(fd, 500, "writing the assets failed (%s); the built-in fonts are used until it succeeds",
                esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "assets written (%zu bytes, %u files)", len, (unsigned)view.count);
    respond(fd, 200, "wrote %zu bytes (%lld sent), %u files, used after a reboot", len, sent, (unsigned)view.count);
    return true;
}

static void handle_firmware(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    if (req->content_length <= 0 || !req->has_sha256) {
        respond(fd, req->content_length < 0 ? 411 : 400, "needs Content-Length and X-Content-SHA256");
        return;
    }
    if (req->content_length > FIRMWARE_MAX) {
        respond(fd, 413, "firmware is larger than an app slot (%d bytes)", FIRMWARE_MAX);
        return;
    }
    size_t       guess = req->deflate ? FIRMWARE_MAX : (size_t)req->content_length;
    memory_ctx_t mem   = {.cap = guess};
    mem.data           = malloc(mem.cap);
    if (!mem.data) {
        respond(fd, 503, "not enough memory for %zu bytes", guess);
        return;
    }
    // begin first: it stops the other tasks, and the body must not start arriving before they are gone (#76)
    hook_begin(OTA_KIND_FIRMWARE, NULL, req->content_length);
    if (req->expect_continue) {
        send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    char sha_hex[OTA_SHA256_HEX + 1];
    progress_base = 0;
    bool ok =
        receive_body(fd, pre, pre_len, req->content_length, req->deflate, FIRMWARE_MAX, memory_sink, &mem, sha_hex);
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        hook_end(false, false);
        free(mem.data);
        if (ok) {
            respond(fd, 400, "SHA-256 mismatch, nothing installed (got %lld bytes, sha256 %s)", last_received, sha_hex);
        } else {
            respond(fd, 400, "transfer failed after %lld of %lld bytes, nothing installed", last_received,
                    req->content_length);
        }
        return;
    }
    hook_end(finish_firmware(fd, mem.data, mem.len, last_received), false);
    free(mem.data);
}

static void handle_file(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    char path[OTA_PATH_MAX + 16];
    if (!file_allowed()) {
        respond(fd, 409, "updating");
        return;
    }
    if (!req->has_path || req->content_length < 0 || !req->has_sha256) {
        respond(fd, req->content_length < 0 ? 411 : 400, "needs X-Path, Content-Length and X-Content-SHA256");
        return;
    }
    if (!ota_path_to_posix(req->path, path, sizeof(path))) {
        respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return;
    }
    if (req->content_length > FILE_MAX) {
        respond(fd, 413, "file is larger than %d bytes", FILE_MAX);
        return;
    }
    // A plain body has its exact size; a compressed one starts from a guess and grows (up to FILE_MAX)
    size_t       guess = req->deflate ? (size_t)req->content_length * 3 : (size_t)req->content_length;
    memory_ctx_t mem   = {.cap = guess > FILE_MAX ? FILE_MAX : (guess ? guess : 1)};
    mem.data           = malloc(mem.cap);
    if (!mem.data) {
        respond(fd, 503, "not enough memory for %lld bytes", req->content_length);
        return;
    }
    hook_begin(OTA_KIND_FILE, display_name(req->path), req->content_length);
    if (req->expect_continue) {
        send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    char sha_hex[OTA_SHA256_HEX + 1];
    progress_base = 0;
    bool ok = receive_body(fd, pre, pre_len, req->content_length, req->deflate, FILE_MAX, memory_sink, &mem, sha_hex);
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        hook_end(false, false);
        free(mem.data);
        if (ok) {
            respond(fd, 400, "SHA-256 mismatch, file left as it was (got %lld bytes, sha256 %s)", last_received, sha_hex);
        } else {
            respond(fd, 400, "transfer failed after %lld of %lld bytes, file left as it was", last_received,
                    req->content_length);
        }
        return;
    }
    hook_end(finish_file(fd, req->path, mem.data, mem.len, last_received), false);
    free(mem.data);
}

// A large upload arriving in pieces: each piece is checked on its own and resent alone if damaged; the whole file is
// collected in memory and checked once more before it is written or installed
static struct {
    bool        active;
    ota_route_t route;
    char        path[OTA_PATH_MAX + 1];
    char        file_sha256[OTA_SHA256_HEX + 1];
    uint8_t    *data;
    size_t      total;
    size_t      got;
    long long   sent;
} staged;

static void staged_reset(void) {
    free(staged.data);
    memset(&staged, 0, sizeof(staged));
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} fixed_ctx_t;

static bool fixed_sink(void *ctx, uint8_t const *data, size_t len) {
    fixed_ctx_t *buf = ctx;
    if (buf->len + len > buf->cap) {
        return false;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    return true;
}

static void handle_piece(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    long long max = req->route == OTA_ROUTE_FIRMWARE ? FIRMWARE_MAX : req->route == OTA_ROUTE_ASSETS ? ASSETS_MAX : FILE_MAX;
    char      posix[OTA_PATH_MAX + 16];
    if (req->route == OTA_ROUTE_FILE && !file_allowed()) { // firmware and assets come after /prepare
        respond(fd, 409, "updating");
        return;
    }
    if (!req->has_sha256 || req->content_length <= 0 || req->total_size <= 0 || !req->has_file_sha256 ||
        req->deflate) {
        respond(fd, 400, "a piece needs Content-Length, X-Content-SHA256, X-Total-Size, X-File-SHA256, no compression");
        return;
    }
    if (req->total_size > max) {
        respond(fd, 413, "larger than %lld bytes", max);
        return;
    }
    if (req->route == OTA_ROUTE_FILE && (!req->has_path || !ota_path_to_posix(req->path, posix, sizeof(posix)))) {
        respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return;
    }
    char const *path = req->route == OTA_ROUTE_FILE ? req->path : "";
    if (req->offset == 0) {
        staged_reset();
        staged.data = malloc((size_t)req->total_size);
        if (!staged.data) {
            respond(fd, 503, "not enough memory for %lld bytes", req->total_size);
            return;
        }
        staged.active = true;
        hook_begin(req->route == OTA_ROUTE_FIRMWARE ? OTA_KIND_FIRMWARE
                   : req->route == OTA_ROUTE_ASSETS ? OTA_KIND_ASSETS
                                                    : OTA_KIND_FILE,
                   req->route == OTA_ROUTE_FILE     ? display_name(req->path)
                   : req->route == OTA_ROUTE_ASSETS ? "assets"
                                                    : NULL,
                   req->total_size);
        staged.route  = req->route;
        staged.total  = (size_t)req->total_size;
        snprintf(staged.path, sizeof(staged.path), "%s", path);
        strcpy(staged.file_sha256, req->file_sha256);
    } else if (!staged.active || staged.route != req->route || staged.total != (size_t)req->total_size ||
               strcmp(staged.path, path) != 0 || strcmp(staged.file_sha256, req->file_sha256) != 0 ||
               (size_t)req->offset != staged.got) {
        respond(fd, 409, "expected the piece at offset %zu", staged.active ? staged.got : (size_t)0);
        return;
    }
    if (req->offset + req->content_length > req->total_size) {
        respond(fd, 400, "piece runs past X-Total-Size");
        return;
    }
    if (req->expect_continue) {
        send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }

    progress_base = (uint32_t)staged.got;
    fixed_ctx_t piece = {.data = staged.data + staged.got, .cap = staged.total - staged.got};
    char        sha_hex[OTA_SHA256_HEX + 1];
    bool        ok = receive_body(fd, pre, pre_len, req->content_length, false, req->content_length, fixed_sink, &piece,
                                  sha_hex);
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        respond(fd, 400, "piece at %lld %s, send it again", req->offset, ok ? "arrived damaged" : "didn't arrive");
        return; // staged.got is unchanged, so the same piece can come again
    }
    staged.got  += (size_t)req->content_length;
    staged.sent += req->content_length;
    if (staged.got < staged.total) {
        respond(fd, 200, "piece ok, %zu of %zu bytes", staged.got, staged.total);
        return;
    }

    sha256_ctx_t whole;
    uint8_t      digest[SHA256_DIGEST_LEN];
    sha256_init(&whole);
    sha256_update(&whole, staged.data, staged.total);
    sha256_final(&whole, digest);
    ota_hex(digest, sizeof(digest), sha_hex);
    if (strcmp(sha_hex, staged.file_sha256) != 0) {
        hook_end(false, false);
        staged_reset();
        respond(fd, 400, "whole-file SHA-256 mismatch (sha256 %s), nothing written", sha_hex);
        return;
    }
    bool finished = staged.route == OTA_ROUTE_FIRMWARE ? finish_firmware(fd, staged.data, staged.total, staged.sent)
                    : staged.route == OTA_ROUTE_ASSETS ? finish_assets(fd, staged.data, staged.total, staged.sent)
                                                       : finish_file(fd, staged.path, staged.data, staged.total, staged.sent);
    hook_end(finished, false);
    staged_reset();
}

static void handle_delete(int fd, ota_request_t const *req) {
    char path[OTA_PATH_MAX + 16];
    if (!req->has_path) {
        respond(fd, 400, "needs X-Path");
        return;
    }
    if (!ota_path_to_posix(req->path, path, sizeof(path))) {
        respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return;
    }
    if (unlink(path) != 0) {
        respond(fd, 404, "couldn't delete %s", req->path);
        return;
    }
    respond(fd, 200, "deleted %s", req->path);
}

// GET /screenshot (#30): the screen as a BMP. The picture is rendered by the hook (LVGL stays out of this component),
// sent from its PSRAM buffer in slices and freed. It is never logged: it can show private messages.
static void handle_screenshot(int fd) {
    if (!hooks.screenshot) {
        respond(fd, 404, "no screen here");
        return;
    }
    if (!file_allowed()) {
        respond(fd, 409, "a firmware update is running");
        return;
    }
    uint8_t const *pixels;
    uint32_t       w, h, stride;
    int            status = hooks.screenshot(&pixels, &w, &h, &stride);
    if (status) {
        respond(fd, status, status == 503 ? "the screen is busy, try again" : "no screenshot");
        return;
    }
    uint8_t head[OTA_BMP_HEADER_LEN];
    size_t  row  = (size_t)w * 2;
    size_t  size = ota_bmp565_file_size(w, h);
    // Rows go out as they sit in memory: only a tightly packed picture with 4-byte rows is supported
    if (!size || stride != row || ota_bmp565_row_bytes(w) != row || !ota_bmp565_header(w, h, head)) {
        respond(fd, 500, "unsupported screen shape");
    } else {
        struct timeval tv = {.tv_sec = SHOT_SEND_S};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char reply[160];
        int  n = snprintf(reply, sizeof(reply),
                          "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
                          "Connection: close\r\n\r\n",
                          (unsigned)size);
        bool ok = send_all(fd, reply, (size_t)n) && send_all(fd, head, sizeof(head));
        long stop = now_s() + SHOT_TOTAL_S;
        for (size_t off = 0, total = row * h; ok && off < total; off += SHOT_SLICE) {
            if (now_s() > stop) {
                break;
            }
            ok = send_all(fd, pixels + off, total - off < SHOT_SLICE ? total - off : SHOT_SLICE);
        }
    }
    if (hooks.screenshot_free) {
        hooks.screenshot_free();
    }
}

// Handle one connection. Returns true when the badge should reboot.
static bool handle_connection(int fd) {
    size_t len      = 0;
    size_t head_len = 0;
    long   deadline = now_s() + HEAD_DEADLINE_S;
    while (!head_len) {
        if (now_s() > deadline) {
            return false;
        }
        if (len == sizeof(head_buf)) {
            respond(fd, 400, "request head too large");
            return false;
        }
        ssize_t n = read(fd, head_buf + len, sizeof(head_buf) - len);
        if (n <= 0) {
            return false;
        }
        len      += (size_t)n;
        head_len  = ota_head_length(head_buf, len);
    }

    ota_request_t req;
    int           status = ota_parse_request(head_buf, head_len, &req);
    if (status) {
        respond(fd, status, "bad request");
        return false;
    }

    if (req.route == OTA_ROUTE_INFO) {
        // "deflate chunks" tells tools/badge.sh this receiver takes compressed bodies and pieces
        respond(fd, 200, "badge_fw %s; receiver: deflate chunks%s", esp_app_get_description()->version,
                hooks.prepare ? " prepare" : "");
        return false;
    }

    if (!req.has_token || !ota_token_equal(req.token, token)) {
        respond(fd, 401, "wrong or missing token");
        return false;
    }

    uint8_t const *pre     = (uint8_t const *)head_buf + head_len;
    size_t         pre_len = len - head_len;
    switch (req.route) {
        case OTA_ROUTE_FIRMWARE:
            if (req.has_offset) {
                handle_piece(fd, &req, pre, pre_len);
            } else {
                handle_firmware(fd, &req, pre, pre_len);
            }
            return false;
        case OTA_ROUTE_FILE:
            if (req.has_offset) {
                handle_piece(fd, &req, pre, pre_len);
            } else {
                handle_file(fd, &req, pre, pre_len);
            }
            return false;
        case OTA_ROUTE_ASSETS:
            if (req.has_offset) {
                handle_piece(fd, &req, pre, pre_len);
            } else {
                respond(fd, 400, "send assets in pieces (X-Offset, X-Total-Size, X-File-SHA256)");
            }
            return false;
        case OTA_ROUTE_PREPARE:
            if (!hooks.prepare) {
                respond(fd, 404, "no prepare here");
                return false;
            }
            hooks.prepare();
            vTaskDelay(pdMS_TO_TICKS(300)); // freed stacks go back to the heap from the idle task
            respond(fd, 200, "ready dma_free=%u dma_min=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
            return false;
        case OTA_ROUTE_DELETE: handle_delete(fd, &req); return false;
        case OTA_ROUTE_SCREENSHOT: handle_screenshot(fd); return false;
        case OTA_ROUTE_REBOOT:
            respond(fd, 200, "rebooting");
            hook_end(true, firmware_installed); // a plain reboot with no new firmware is not an update
            return true;
        default: return false;
    }
}

static bool load_or_create_token(void) {
    FILE *f = fopen(TOKEN_FILE, "r");
    if (f) {
        char text[OTA_TOKEN_LEN + 8] = {0};
        fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[OTA_TOKEN_LEN] = '\0';
        bool valid          = true;
        for (int i = 0; i < OTA_TOKEN_LEN; i++) {
            char c = text[i];
            valid  = valid && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (valid) {
            strcpy(token, text);
            return true;
        }
    }
    // The P4 has no radio of its own, so the RNG only draws on real entropy while the ADC source is on. The token
    // guards firmware installs: it must not be predictable.
    uint8_t raw[OTA_TOKEN_LEN / 2];
    bootloader_random_enable();
    esp_fill_random(raw, sizeof(raw));
    bootloader_random_disable();
    ota_hex(raw, sizeof(raw), token);
    f = fopen(TOKEN_FILE, "w");
    if (!f) {
        return false;
    }
    bool written = fprintf(f, "%s\n", token) > 0;
    return fclose(f) == 0 && written;
}

static void server_task(void *arg) {
    (void)arg;
    // Wi-Fi may come up late or not at all (no credentials): wait for an address before listening
    while (!wifi_has_ip(NULL, 0)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ESP_LOGE(TAG, "can't create a socket");
        vTaskDelete(NULL);
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(OTA_PORT), .sin_addr.s_addr = 0};
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d", OTA_PORT);
        close(ls);
        vTaskDelete(NULL);
    }
    ESP_LOGW(TAG, "update server on port %d", OTA_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int                fd       = accept(ls, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        set_read_timeout(fd, READ_TIMEOUT_S, 0);
        bool reboot = handle_connection(fd);

        // Let the client close first so the TIME_WAIT lands on its side, not on our port
        set_read_timeout(fd, 0, 500000);
        for (int i = 0; i < 16 && read(fd, body_buf, sizeof(body_buf)) > 0; i++) {
        }
        close(fd);
        if (reboot) {
            ESP_LOGW(TAG, "reboot requested over Wi-Fi");
            vTaskDelay(pdMS_TO_TICKS(300));
            system_restart();
        }
    }
}

static void cmd_token(char const *args) {
    (void)args;
    console_reply("token %s", token[0] ? token : "(none: storage unavailable?)");
}

void ota_server_start(void) {
    console_register("token", "token (the update token for tools/badge.sh)", cmd_token);
    if (!load_or_create_token()) {
        ESP_LOGE(TAG, "couldn't save the token in %s; it changes on every boot", TOKEN_FILE);
    }
    if (xTaskCreatePinnedToCore(server_task, "ota", SERVER_STACK, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "update server task failed to start");
    }
}
