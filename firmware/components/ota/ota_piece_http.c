// Uploads in pieces (#447, from ota_server.c): PUT /firmware, /file and /assets with X-Offset. Each piece is checked
// on its own and resent alone if damaged; firmware goes straight into the inactive slot (#37), the rest is staged in
// PSRAM; the whole file is checked once more before it is written or installed.
#include "ota_server_priv.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "flash_chunk.h"
#include "ota_path.h"
#include "ota_pieces.h"
#include "ota_valid.h"
#include "sdkconfig.h"
#include "sha256.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char const TAG[] = "ota";

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
    ota_pieces_t pieces; // SHA-256 of each verified piece, to find damaged ones if the whole file doesn't match
    // Firmware pieces are not staged in memory (#37): each goes straight into the inactive slot, which is read back
    // and hashed at the end. data stays NULL then.
    esp_partition_t const *slot;
    esp_ota_handle_t       ota;
    bool                   ota_open;
    bool                   dirty; // the last attempt at the current piece wrote (part of) it: erase before the next
    long                   last_s; // when the last piece came: an upload left half-way is dropped (ota_piece_expire)
} staged;

// Whole cache lines, so no neighbouring allocation or DMA driver shares a line with the staging buffer (#37)
#define STAGE_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE

static void staged_reset(void) {
    if (staged.ota_open) {
        esp_ota_abort(staged.ota);
    }
    heap_caps_free(staged.data);
    ota_pieces_clear(&staged.pieces);
    memset(&staged, 0, sizeof(staged));
}

// An upload in pieces that stopped coming holds its staging buffer (up to FILE_MAX of PSRAM), an open slot and the
// stopped screens: give it all back after PIECE_IDLE_S without a piece (#5)
#define PIECE_IDLE_S 300

void ota_piece_expire(bool now) {
    if (staged.active && (now || ota_now_s() - staged.last_s > PIECE_IDLE_S)) {
        ESP_LOGW(TAG, "upload in pieces abandoned at %zu of %zu bytes: dropped", staged.got, staged.total);
        hook_end(false, false);
        staged_reset();
    }
}

// A partition read for ota_hash_range; buf is always an internal bounce buffer (#364)
static int slot_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    return esp_partition_read(ctx, off, buf, n) == ESP_OK ? 0 : -1;
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

// The streamed firmware piece goes to the slot at base + what came before in this piece
typedef struct {
    esp_ota_handle_t handle;
    size_t           base;
    size_t           len;
} flash_ctx_t;

static bool flash_sink(void *ctx, uint8_t const *data, size_t len) {
    flash_ctx_t *f = ctx;
    if (esp_ota_write_with_offset(f->handle, data, len, (uint32_t)(f->base + f->len)) != ESP_OK) {
        return false;
    }
    f->len += len;
    return true;
}

// Open the inactive slot for a streamed image. esp_ota_begin erases only what the first piece covers; every later
// piece (and a resent one) is erased by stream_erase before it arrives, so no single call blocks for long.
static esp_err_t stream_open(size_t first_len) {
    staged.slot   = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = staged.slot ? esp_ota_begin(staged.slot, first_len, &staged.ota) : ESP_ERR_NOT_FOUND;
    staged.ota_open = err == ESP_OK;
    return err;
}

// Make sure at least want bytes of the body are in *pre (reading them from the socket if needed), so the image header
// can be checked before esp_ota_begin erases anything
static uint8_t peek_buf[OTA_IMAGE_HEADER_LEN];

static bool peek_body(int fd, uint8_t const **pre, size_t *pre_len, size_t want) {
    if (*pre_len >= want) {
        return true;
    }
    size_t have = *pre_len;
    memcpy(peek_buf, *pre, have);
    while (have < want) {
        ssize_t n = read(fd, peek_buf + have, want - have);
        if (n <= 0) {
            return false;
        }
        have += (size_t)n;
    }
    *pre     = peek_buf;
    *pre_len = have;
    return true;
}

// The clamp below keeps an erase inside the slot; it only hides nothing while no accepted piece ends past the slot,
// i.e. while the firmware size limit is the slot size (#403; OTA_SLOT_SIZE comes from the partition table, CMakeLists)
_Static_assert(FIRMWARE_MAX == OTA_SLOT_SIZE, "FIRMWARE_MAX must equal the app slot size in partitions.csv");

static bool stream_erase(size_t offset, size_t len) {
    size_t end = ota_round_up(offset + len, 4096);
    if (end == 0 || end < offset) { // ota_round_up's overflow sentinel; pieces lie inside the slot, so never on the badge
        return false;
    }
    if (end > staged.slot->size) {
        end = staged.slot->size;
    }
    return esp_partition_erase_range(staged.slot, offset, end - offset) == ESP_OK;
}

// The streamed image is complete: hash what the slot holds (not what was received), and install it if it is the file.
// noinline, like rehash_piece and ota_finish_assets: each has a bounce buffer, which would cross the 1 KB frame line when
// folded into handle_piece
__attribute__((noinline)) static bool finish_streamed(int fd, char sha_hex[OTA_SHA256_HEX + 1]) {
    uint8_t digest[SHA256_DIGEST_LEN];
    uint8_t bounce[FLASH_CHUNK_BOUNCE]; // internal, on the server's stack: not ota_body_buf (#364)
    if (ota_hash_range(slot_read, (void *)staged.slot, 0, staged.total, bounce, sizeof(bounce), digest) != 0) {
        ota_respond(fd, 500, "couldn't read the slot back, nothing installed");
        return false;
    }
    ota_hex(digest, sizeof(digest), sha_hex);
    if (!ota_whole_file_ok(staged.got, staged.total, sha_hex, staged.file_sha256)) {
        size_t bad[8];
        size_t n = ota_pieces_check_read(&staged.pieces, slot_read, (void *)staged.slot, bounce, sizeof(bounce), bad,
                                         8);
        ESP_LOGW(TAG, "whole-file mismatch in the slot: %zu of %zu pieces differ from what was received", n,
                 staged.pieces.n);
        for (size_t i = 0; i < n && i < 8; i++) {
            ota_piece_t const *pc = &staged.pieces.v[bad[i]];
            ESP_LOGW(TAG, "  piece %zu at offset %u (%u bytes) differs", bad[i], (unsigned)pc->offset, (unsigned)pc->len);
        }
        ota_respond(fd, 400, "whole-file SHA-256 mismatch (sha256 %s), nothing installed", sha_hex);
        return false;
    }
    staged.ota_open = false; // esp_ota_end ends the handle, good or bad
    return ota_install_slot(fd, staged.slot, staged.ota, staged.total, staged.sent);
}

// The whole file didn't match: say which verified pieces no longer match what was stored for them
static void report_damaged_pieces(void) {
    size_t bad[8];
    size_t n = ota_pieces_check(&staged.pieces, staged.data, staged.total, bad, 8);
    ESP_LOGW(TAG, "whole-file mismatch: staging buffer %p, %zu of %zu pieces changed in PSRAM since they were checked",
             (void *)staged.data, n, staged.pieces.n);
    for (size_t i = 0; i < n && i < 8; i++) {
        ota_piece_t const *pc = &staged.pieces.v[bad[i]];
        ESP_LOGW(TAG, "  piece %zu at offset %u (%u bytes) changed", bad[i], (unsigned)pc->offset, (unsigned)pc->len);
    }
}

// Hash a received piece again where it ended up: in the PSRAM staging buffer, or read back from the OTA slot (#37)
__attribute__((noinline)) static void rehash_piece(size_t offset, size_t len, uint8_t digest[SHA256_DIGEST_LEN]) {
    uint8_t bounce[FLASH_CHUNK_BOUNCE]; // internal, on the server's stack: not ota_body_buf (#364)
    if (staged.data) {
        sha256_ctx_t again;
        sha256_init(&again);
        sha256_update(&again, staged.data + offset, len);
        sha256_final(&again, digest);
    } else if (ota_hash_range(slot_read, (void *)staged.slot, offset, len, bounce, sizeof(bounce), digest) != 0) {
        memset(digest, 0, SHA256_DIGEST_LEN); // unreadable: never equals the sender's hash
    }
}

// Offset 0 starts a transfer: get its storage ready and tell the screen. A firmware image's header is read (and
// 100 Continue sent, *continued) before the slot is opened. False = answered already.
static bool piece_start(int fd, ota_request_t const *req, char const *path, uint8_t const **pre, size_t *pre_len,
                        bool *continued) {
    bool stream = req->route == OTA_ROUTE_FIRMWARE;
    staged_reset();
    if (req->route == OTA_ROUTE_FILE &&
        !ota_storage_room(fd, (uint64_t)req->total_size, *pre_len, req->expect_continue ? 0 : req->content_length)) {
        return false;
    }
    if (!stream) {
        size_t cap  = ota_round_up((size_t)req->total_size, STAGE_ALIGN); // 0 only on overflow: never allocated
        staged.data = cap ? heap_caps_aligned_alloc(STAGE_ALIGN, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!staged.data) {
            ota_respond(fd, 503, "not enough memory for %lld bytes", req->total_size);
            return false;
        }
    }
    staged.active = true;
    hook_begin(req->route == OTA_ROUTE_FIRMWARE ? OTA_KIND_FIRMWARE
               : req->route == OTA_ROUTE_ASSETS ? OTA_KIND_ASSETS
                                                : OTA_KIND_FILE,
               req->route == OTA_ROUTE_FILE     ? ota_display_name(req->path)
               : req->route == OTA_ROUTE_ASSETS ? "assets"
                                                : NULL,
               req->total_size);
    staged.route = req->route;
    staged.total = (size_t)req->total_size;
    snprintf(staged.path, sizeof(staged.path), "%s", path);
    strcpy(staged.file_sha256, req->file_sha256);
    // After hook_begin: it stops the other tasks before the slot is touched
    if (stream) {
        if (req->expect_continue) {
            ota_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            *continued = true;
        }
        // Offset 0 of firmware is never compressed (ota_piece_headers), so these are the image's own first bytes
        size_t want = req->content_length < OTA_IMAGE_HEADER_LEN ? (size_t)req->content_length : OTA_IMAGE_HEADER_LEN;
        if (!peek_body(fd, pre, pre_len, want) || !ota_image_header_ok(*pre, *pre_len, CONFIG_IDF_FIRMWARE_CHIP_ID)) {
            hook_end(false, false);
            staged_reset();
            ota_respond(fd, 422, "not an app image for this chip, nothing erased");
            return false;
        }
        esp_err_t err = stream_open((size_t)req->content_length);
        if (err != ESP_OK) {
            hook_end(false, false);
            staged_reset();
            ota_respond_begin_failed(fd, err);
            return false;
        }
    }
    return true;
}

// hold: the request that completed the file said X-Reboot: hold, so an installed image waits for the sender's reboot
static void piece_done(int fd, bool hold) {
    uint8_t digest[SHA256_DIGEST_LEN];
    char    sha_hex[OTA_SHA256_HEX + 1];
    bool    finished;
    if (staged.route == OTA_ROUTE_FIRMWARE) {
        finished = finish_streamed(fd, sha_hex);
        hook_end(finished, finished && !hold);
        staged_reset();
        return;
    }
    sha256_ctx_t whole;
    sha256_init(&whole);
    sha256_update(&whole, staged.data, staged.total);
    sha256_final(&whole, digest);
    ota_hex(digest, sizeof(digest), sha_hex);
    if (strcmp(sha_hex, staged.file_sha256) != 0) {
        report_damaged_pieces();
        hook_end(false, false);
        staged_reset();
        ota_respond(fd, 400, "whole-file SHA-256 mismatch (sha256 %s), nothing written", sha_hex);
        return;
    }
    finished = staged.route == OTA_ROUTE_ASSETS ? ota_finish_assets(fd, staged.data, staged.total, staged.sent)
                                                : ota_finish_file(fd, staged.path, staged.data, staged.total, staged.sent);
    hook_end(finished, finished && staged.route == OTA_ROUTE_ASSETS && !hold);
    staged_reset();
}

// Receive one piece into its place: the staging buffer, or the slot (erased first when it is not fresh). A compressed
// piece (#298) is decoded on its way in, capped at its X-Piece-Size, so len below is always the decoded length.
static bool piece_receive(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len, bool continued) {
    bool        stream = req->route == OTA_ROUTE_FIRMWARE;
    size_t      len    = (size_t)ota_piece_len(req); // ota_piece_bounds: inside the file, so below 4 MB
    char        sha_hex[OTA_SHA256_HEX + 1];
    flash_ctx_t flash = {.handle = staged.ota, .base = staged.got};
    fixed_ctx_t mem   = {.data = stream ? NULL : staged.data + staged.got, .cap = len};
    if (stream && (req->offset > 0 || staged.dirty) && !stream_erase(staged.got, len)) {
        staged.dirty = true;
        ota_respond(fd, 500, "couldn't erase the slot for the piece at %lld, send it again", req->offset);
        return false;
    }
    staged.dirty = stream; // from here on the slot holds (part of) this piece until it is verified
    if (req->expect_continue && !continued) {
        ota_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    ota_progress_base = (uint32_t)staged.got;
    body_t body   = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                     .max = (long long)len, .sink = stream ? flash_sink : fixed_sink,
                     .ctx = stream ? (void *)&flash : (void *)&mem, .piece = true};
    bool   ok     = ota_receive_body(fd, body, sha_hex); // a compressed piece: exactly len bytes (ota_inflate_complete)
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        ota_respond(fd, 400, "piece at %lld %s, send it again", req->offset, ok ? "arrived damaged" : "didn't arrive");
        return false; // staged.got is unchanged, so the same piece can come again
    }
    // The piece was hashed on its way through the internal buffer; hash it again where it landed (#37)
    uint8_t stored[SHA256_DIGEST_LEN];
    rehash_piece(staged.got, len, stored);
    ota_hex(stored, sizeof(stored), sha_hex);
    if (strcmp(sha_hex, req->sha256) != 0) {
        ESP_LOGW(TAG, "piece at %lld damaged in %s, asking for it again", req->offset, stream ? "the slot" : "PSRAM");
        ota_respond(fd, 400, "piece at %lld was damaged in the badge's memory, send it again", req->offset);
        return false;
    }
    staged.dirty = false;
    ota_pieces_add(&staged.pieces, staged.got, len, stored);
    staged.got  += len;
    staged.sent += req->content_length; // bytes on the wire, as the final reply says
    return true;
}

// Answer a refused piece with the text it always had; false (nothing sent) for OTA_PIECE_OK
static bool piece_verdict_sent(int fd, ota_piece_verdict_t verdict, long long max) {
    switch (verdict) {
        case OTA_PIECE_MISSING:
            ota_respond(fd, 400, "a piece needs Content-Length, X-Content-SHA256, X-Total-Size and X-File-SHA256");
            return true;
        case OTA_PIECE_BAD_DEFLATE:
            ota_respond(fd, 400, "a compressed piece needs X-Piece-Size (its decoded bytes), and only a compressed piece has one");
            return true;
        case OTA_PIECE_FIRST_PLAIN:
            ota_respond(fd, 400, "the first firmware piece comes uncompressed: its image header is checked before anything is erased");
            return true;
        case OTA_PIECE_BLOATED: ota_respond(fd, 413, "compressed piece larger than its X-Piece-Size allows"); return true;
        case OTA_PIECE_ZTOO_LARGE:
            ota_respond(fd, 413, "a compressed piece decodes to at most %d bytes", OTA_ZPIECE_MAX);
            return true;
        case OTA_PIECE_TOO_LARGE: ota_respond(fd, 413, "larger than %lld bytes", max); return true;
        case OTA_PIECE_UNALIGNED:
            ota_respond(fd, 400, "firmware pieces start on a 4096-byte boundary and are whole multiples of it, except the last");
            return true;
        case OTA_PIECE_PAST_END: ota_respond(fd, 400, "piece runs past X-Total-Size"); return true;
        default: return false;
    }
}

// Every check a piece passes before anything is touched, answered in the order they always were; true = refused.
// The bounds come last but before piece_start() shows the update screen (#125).
static bool piece_refused(int fd, ota_request_t const *req, char *posix, size_t posix_cap) {
    long long max = req->route == OTA_ROUTE_FIRMWARE ? FIRMWARE_MAX : req->route == OTA_ROUTE_ASSETS ? ASSETS_MAX : FILE_MAX;
    bool      file = req->route == OTA_ROUTE_FILE;
    if (file && !file_allowed()) { // firmware and assets come after /prepare
        ota_respond(fd, 409, "updating");
        return true;
    }
    if (piece_verdict_sent(fd, ota_piece_headers(req, max, 4096), max)) {
        return true;
    }
    if (file && (!req->has_path || !ota_path_to_posix(req->path, posix, posix_cap))) {
        ota_respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return true;
    }
    if (file && ota_is_directory(posix)) {
        ota_respond(fd, 409, "%s is a directory, nothing written", req->path);
        return true;
    }
    return piece_verdict_sent(fd, ota_piece_bounds(req), max);
}

static void handle_piece(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    char posix[OTA_PATH_MAX + 16];
    if (piece_refused(fd, req, posix, sizeof(posix))) {
        return;
    }
    char const              *path      = req->route == OTA_ROUTE_FILE ? req->path : "";
    bool                     continued = false;
    ota_piece_expect_t const expect    = {
           .active      = staged.active,
           .route       = staged.route,
           .total       = staged.total,
           .got         = staged.got,
           .path        = staged.path,
           .file_sha256 = staged.file_sha256,
    };
    if (req->offset == 0) {
        if (!piece_start(fd, req, path, &pre, &pre_len, &continued)) {
            return;
        }
    } else if (!ota_piece_is_next(&expect, req, path)) {
        ota_respond(fd, 409, "expected the piece at offset %zu", staged.active ? staged.got : (size_t)0);
        return;
    }
    staged.last_s = ota_now_s();
    if (!piece_receive(fd, req, pre, pre_len, continued)) {
        return;
    }
    if (staged.got < staged.total) {
        ota_respond(fd, 200, "piece ok, %zu of %zu bytes", staged.got, staged.total);
        return;
    }
    piece_done(fd, req->hold_reboot);
}

// The staged upload is the refused request's own: the same route and, for a held file, the same file however it was named
bool ota_piece_staged_is(ota_request_t const *req, bool held_file, char const *posix) {
    char mine[OTA_PATH_MAX + 16];
    return staged.active && staged.route == req->route &&
           (!held_file || (ota_path_to_posix(staged.path, mine, sizeof(mine)) && strcmp(mine, posix) == 0));
}

bool ota_piece_active(void) {
    return staged.active;
}

bool ota_on_piece(ota_conn_t const *conn) {
    handle_piece(conn->fd, conn->req, conn->pre, conn->pre_len);
    return false;
}

bool ota_on_assets(ota_conn_t const *conn) {
    ota_respond(conn->fd, 400, "send assets in pieces (X-Offset, X-Total-Size, X-File-SHA256)");
    return false;
}
