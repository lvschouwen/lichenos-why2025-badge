// Whole uploads and deletes of the update server (#447, from ota_server.c): PUT /firmware and PUT /file received into
// memory and checked before anything touches flash (#40), DELETE /file, and the finishing steps the pieces share
// (a file put in place, an image installed, the assets written).
#include "ota_server_priv.h"

#include "assets.h"
#include "assets_fmt.h"
#include "esp_log.h"
#include "flash_chunk.h"
#include "ota_hold.h"
#include "ota_path.h"
#include "ota_pieces.h"
#include "ota_trial.h"
#include "ota_valid.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "ota";

// The file's name as the screen shows it: what follows the last ':' or '/'
char const *ota_display_name(char const *vms) {
    char const *name = vms;
    for (char const *p = vms; *p; p++) {
        if (*p == ':' || *p == '/') {
            name = p + 1;
        }
    }
    return *name ? name : vms;
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} memory_ctx_t;

static bool memory_sink(void *ctx, uint8_t const *data, size_t len) {
    memory_ctx_t *mem = ctx;
    if (mem->len + len > mem->cap) {
        // Only compressed bodies grow past their first guess; ota_receive_body caps the total
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
    for (char *p = dir + strlen(STORAGE_BASE "/"); *p; p++) {
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

// Room for an upload of size bytes on /storage (#299), else a 507 now instead of a failed write after the transfer.
// A drive whose free space can't be read is let through: the write reports that.
// body: bytes of a request body not read yet (0: none, or the client waits for 100 Continue), dropped before the reply.
bool ota_storage_room(int fd, uint64_t size, size_t pre_len, long long body) {
    uint64_t free_b, total_b;
    if (!storage_free_bytes(&free_b, &total_b) || ota_file_fits(free_b, size, STORAGE_CLUSTER)) {
        return true;
    }
    ota_drain_body(fd, pre_len, body);
    ota_respond(fd, 507, "not enough space on the internal storage: need %llu bytes, %llu free, nothing written",
                (unsigned long long)size, (unsigned long long)free_b);
    return false;
}

// Put a checked file in place: write it next to the target, then swap it in. vms is the path as badge.sh names it.
bool ota_finish_file(int fd, char const *vms, uint8_t const *data, size_t len, long long sent) {
    char path[OTA_PATH_MAX + 16];
    char temp[sizeof(path) + sizeof(OTA_TEMP_NAME)];
    if (!ota_path_to_posix(vms, path, sizeof(path)) || !ota_temp_to_posix(vms, temp, sizeof(temp))) {
        ota_respond(fd, 400, "bad path");
        return false;
    }
    if (!ota_storage_room(fd, len, 0, 0) || !ota_file_change_begin(fd, path)) {
        return false; // before the bundle is marked changed: the old file stays
    }
    mkdir_parents(path);
    // The change stays marked in progress until the rename has landed or failed: "fw onair on" in between must see it
    // (#454). A C6 file's change also stays pending through c6link_bundle_changed.
    if (!write_file(temp, data, len)) {
        ota_change_end(false);
        unlink(temp);
        ota_respond(fd, 500, "couldn't write %s, file left as it was", temp);
        return false;
    }
    // FLASH0: is /storage, LittleFS since #300: one rename replaces the old file atomically (a cut leaves the old file
    // or the new one). A target another task has open stays put: esp_littlefs refuses the rename with EBUSY.
    int moved = rename(temp, path);
    int err   = errno;
    ota_change_end(false);
    if (moved != 0) {
        unlink(temp);
        ota_respond(fd, 500, "couldn't move the upload into place (%s), file left as it was", strerror(err));
        return false;
    }
    ESP_LOGI(TAG, "wrote %s (%zu bytes)", path, len);
    ota_respond(fd, 200, "wrote %zu bytes (%lld sent) to %s", len, sent, vms);
    return true;
}

// Validate what was written to the slot and make it the boot slot; it runs after a reboot, on trial until it proves itself
bool ota_install_slot(int fd, esp_partition_t const *slot, esp_ota_handle_t handle, size_t len, long long sent) {
    esp_err_t err = esp_ota_end(handle); // checks the image: chip, segments, hash
    if (err != ESP_OK) {
        ota_respond(fd, 422, "image failed validation (%s), nothing installed", esp_err_to_name(err));
        return false;
    }
    if (ota_hold_refused(fd, OTA_HOLD_BOOT, ota_change_begin())) { // the hold may have gone on while the image came (#420)
        return false;
    }
    err = esp_ota_set_boot_partition(slot);
    ota_change_end(err == ESP_OK);
    if (err != ESP_OK) {
        ota_respond(fd, 500, "couldn't select %s (%s)", slot->label, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "firmware installed in %s (%zu bytes)", slot->label, len);
    ota_respond(fd, 200, "installed %zu bytes (%lld sent), runs after a reboot", len, sent);
    ota_firmware_installed = true;
    return true;
}

// esp_ota_begin refused: say why when the reason is the running image still being on trial (#5)
void ota_respond_begin_failed(int fd, esp_err_t err) {
    if (ota_image_trial_pending()) {
        ota_respond(fd, 503, "%s", TRIAL_REFUSAL);
    } else {
        ota_respond(fd, 503, "couldn't start an OTA session (%s)", esp_err_to_name(err));
    }
}

// Install a checked firmware image (received whole in memory) into the other app slot
static bool finish_firmware(int fd, uint8_t const *data, size_t len, long long sent) {
    if (!ota_image_header_ok(data, len, CONFIG_IDF_FIRMWARE_CHIP_ID)) {
        ota_respond(fd, 422, "not an app image for this chip, nothing erased or installed");
        return false;
    }
    esp_partition_t const *slot = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t       handle;
    esp_err_t              err  = slot ? esp_ota_begin(slot, len, &handle) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        ota_respond_begin_failed(fd, err);
        return false;
    }
    if (esp_ota_write(handle, data, len) != ESP_OK) {
        esp_ota_abort(handle);
        ota_respond(fd, 500, "flash write failed, nothing installed");
        return false;
    }
    return ota_install_slot(fd, slot, handle, len, sent);
}

// The same for flash_chunk_same, keeping the error so the reply names it
typedef struct {
    esp_partition_t const *part;
    esp_err_t              err;
} part_read_t;

static int part_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    part_read_t *r = ctx;
    r->err         = esp_partition_read(r->part, off, buf, n);
    return r->err == ESP_OK ? 0 : -1;
}

// Write a checked assets image (#22) over the assets partition. The image is fully checked (header, table, CRC)
// before anything is erased, and read back afterwards. The running firmware uses the boot-time PSRAM copy (#31), which
// assets_unmount drops once the fonts are parsed, and nothing reads the partition until the next boot; still,
// tools/badge.sh asks for /prepare first (the apps and screens stop) and reboots afterwards to load the new image. noinline: its read-back
// bounce buffer stays out of handle_piece's frame (#364).
__attribute__((noinline)) bool ota_finish_assets(int fd, uint8_t const *data, size_t len, long long sent) {
    assets_view_t view;
    assets_err_t  res = assets_open(data, len, true, &view);
    if (res != ASSETS_OK) {
        ota_respond(fd, 422, "not a usable assets image (%s), nothing written", assets_err_str(res));
        return false;
    }
    esp_partition_t const *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASSETS_PARTITION);
    if (!part || len > part->size) {
        ota_respond(fd, 413, "no assets partition, or the image is larger than it");
        return false;
    }
    size_t    erase = (len + part->erase_size - 1) / part->erase_size * part->erase_size;
    esp_err_t err   = esp_partition_erase_range(part, 0, erase);
    if (err == ESP_OK) {
        err = esp_partition_write(part, 0, data, len);
    }
    if (err == ESP_OK) {
        uint8_t     bounce[FLASH_CHUNK_BOUNCE];
        part_read_t r    = {.part = part, .err = ESP_OK};
        int         same = flash_chunk_same(part_read, &r, 0, data, len, bounce, sizeof(bounce));
        err              = same == 1 ? ESP_OK : same == 0 ? ESP_ERR_INVALID_CRC : r.err != ESP_OK ? r.err : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ota_respond(fd, 500, "writing the assets failed (%s); the built-in fonts are used until it succeeds",
                    esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "assets written (%zu bytes, %u files)", len, (unsigned)view.count);
    ota_respond(fd, 200, "wrote %zu bytes (%lld sent), %u files, used after a reboot", len, sent, (unsigned)view.count);
    return true;
}

static void handle_firmware(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    if (!ota_whole_body_ok(req)) {
        ota_respond(fd, 400, "X-Piece-Size belongs to a piece (with X-Offset), nothing installed");
        return;
    }
    if (req->content_length <= 0 || !req->has_sha256) {
        ota_respond(fd, req->content_length < 0 ? 411 : 400, "needs Content-Length and X-Content-SHA256");
        return;
    }
    if (req->content_length > FIRMWARE_MAX) {
        ota_respond(fd, 413, "firmware is larger than an app slot (%d bytes)", FIRMWARE_MAX);
        return;
    }
    size_t       guess = req->deflate ? FIRMWARE_MAX : (size_t)req->content_length;
    memory_ctx_t mem   = {.cap = guess};
    mem.data           = malloc(mem.cap);
    if (!mem.data) {
        ota_respond(fd, 503, "not enough memory for %zu bytes", guess);
        return;
    }
    // begin first: it stops the other tasks, and the body must not start arriving before they are gone (#76)
    hook_begin(OTA_KIND_FIRMWARE, NULL, req->content_length);
    if (req->expect_continue) {
        ota_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    char sha_hex[OTA_SHA256_HEX + 1];
    ota_progress_base = 0;
    body_t body = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                   .max = FIRMWARE_MAX, .sink = memory_sink, .ctx = &mem};
    bool   ok   = ota_receive_body(fd, body, sha_hex);
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        hook_end(false, false);
        free(mem.data);
        if (ok) {
            ota_respond(fd, 400, "SHA-256 mismatch, nothing installed (got %lld bytes, sha256 %s)", ota_last_received, sha_hex);
        } else {
            ota_respond(fd, 400, "transfer failed after %lld of %lld bytes, nothing installed", ota_last_received,
                        req->content_length);
        }
        return;
    }
    // rebooting: the badge restarts into it by itself, unless the sender holds that (X-Reboot: hold, #298)
    bool installed = finish_firmware(fd, mem.data, mem.len, ota_last_received);
    hook_end(installed, installed && !req->hold_reboot);
    free(mem.data);
}

// A push onto a directory name (e.g. FLASH0:c6) must not replace the directory (#5)
bool ota_is_directory(char const *posix_path) {
    struct stat st;
    return stat(posix_path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void handle_file(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    char path[OTA_PATH_MAX + 16];
    if (!file_allowed()) {
        ota_respond(fd, 409, "updating");
        return;
    }
    if (!ota_whole_body_ok(req)) {
        ota_respond(fd, 400, "X-Piece-Size belongs to a piece (with X-Offset), file left as it was");
        return;
    }
    if (!req->has_path || req->content_length < 0 || !req->has_sha256) {
        ota_respond(fd, req->content_length < 0 ? 411 : 400, "needs X-Path, Content-Length and X-Content-SHA256");
        return;
    }
    if (!ota_path_to_posix(req->path, path, sizeof(path))) {
        ota_respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return;
    }
    if (req->content_length > FILE_MAX) {
        ota_respond(fd, 413, "file is larger than %d bytes", FILE_MAX);
        return;
    }
    if (ota_is_directory(path)) {
        ota_respond(fd, 409, "%s is a directory, nothing written", req->path);
        return;
    }
    // A compressed body's size is known once it is in: ota_finish_file checks that one
    if (!req->deflate && !ota_storage_room(fd, (uint64_t)req->content_length, pre_len, req->expect_continue ? 0 : req->content_length)) {
        return;
    }
    // A plain body has its exact size; a compressed one starts from a guess and grows (up to FILE_MAX)
    size_t       guess = req->deflate ? (size_t)req->content_length * 3 : (size_t)req->content_length;
    memory_ctx_t mem   = {.cap = guess > FILE_MAX ? FILE_MAX : (guess ? guess : 1)};
    mem.data           = malloc(mem.cap);
    if (!mem.data) {
        ota_respond(fd, 503, "not enough memory for %lld bytes", req->content_length);
        return;
    }
    hook_begin(OTA_KIND_FILE, ota_display_name(req->path), req->content_length);
    if (req->expect_continue) {
        ota_send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    char sha_hex[OTA_SHA256_HEX + 1];
    ota_progress_base = 0;
    body_t body = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                   .max = FILE_MAX, .sink = memory_sink, .ctx = &mem};
    bool   ok   = ota_receive_body(fd, body, sha_hex);
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        hook_end(false, false);
        free(mem.data);
        if (ok) {
            ota_respond(fd, 400, "SHA-256 mismatch, file left as it was (got %lld bytes, sha256 %s)", ota_last_received, sha_hex);
        } else {
            ota_respond(fd, 400, "transfer failed after %lld of %lld bytes, file left as it was", ota_last_received,
                        req->content_length);
        }
        return;
    }
    hook_end(ota_finish_file(fd, req->path, mem.data, mem.len, ota_last_received), false);
    free(mem.data);
}

static void handle_delete(int fd, ota_request_t const *req) {
    char path[OTA_PATH_MAX + 16];
    if (!req->has_path) {
        ota_respond(fd, 400, "needs X-Path");
        return;
    }
    if (!ota_path_to_posix(req->path, path, sizeof(path))) {
        ota_respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return;
    }
    if (!ota_file_change_begin(fd, path)) {
        return;
    }
    int gone = unlink(path);
    ota_change_end(false);
    if (gone != 0) { // EBUSY: open in another task (esp_littlefs, #300); the file stays (a C6 one's bundle is forgotten)
        int err = errno;
        ota_respond(fd, err == ENOENT ? 404 : 409, "couldn't delete %s: %s", req->path, strerror(err));
        return;
    }
    ota_respond(fd, 200, "deleted %s", req->path);
}

bool ota_on_firmware(ota_conn_t const *conn) {
    handle_firmware(conn->fd, conn->req, conn->pre, conn->pre_len);
    return false;
}

bool ota_on_file(ota_conn_t const *conn) {
    handle_file(conn->fd, conn->req, conn->pre, conn->pre_len);
    return false;
}

bool ota_on_delete(ota_conn_t const *conn) {
    handle_delete(conn->fd, conn->req);
    return false;
}
