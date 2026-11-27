// Whole uploads and deletes of the update server (#447, from ota_server.c): PUT /firmware and PUT /file received into
// memory and checked before anything touches flash (#40), DELETE /file, and the finishing steps the pieces share
// (a file put in place, an image installed, the assets written).
#include "ota_server_priv.h"

#include "assets.h"
#include "assets_fmt.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "flash_chunk.h"
#include "ota_hold.h"
#include "ota_path.h"
#include "ota_pieces.h"
#include "ota_trial.h"
#include "ota_valid.h"
#include "sha256.h"
#include "sdcard.h"
#include "storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[] = "ota";

#define SD_WAIT_MS    2000
#define NOT_ALLOWED   "path not allowed: use FLASH0: or SD0: with a plain file name"

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

// Create every directory leading up to the file at path (a /storage/... or /sd/... path)
void ota_mkdir_parents(char const *path) {
    char dir[OTA_PATH_MAX + 16];
    snprintf(dir, sizeof(dir), "%s", path);
    for (size_t n = ota_path_next_dir(path, 0); n; n = ota_path_next_dir(path, n)) {
        dir[n] = '\0';
        if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "mkdir %s: %s", dir, strerror(errno));
        }
        dir[n] = '/';
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

static int file_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    FILE *f = ctx;
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n ? 0 : -1;
}

#define DIFF_LOG_MAX 8
#define CACHE_LINE   128 // the L2 line the PSRAM is cached in

// A plain read of one byte of the staging buffer, not folded away
static uint8_t peek(uint8_t const *p) {
    return *(volatile uint8_t const *)p;
}

// One byte the file got wrong (#593, the #569 signature: one bit, 0 -> 1), found with plain reads before any msync
typedef struct {
    size_t  off;
    uint8_t in_file, staging;
} flip_t;

// The staging byte again after its cache line is written back and invalidated: tells a read-path transient during the
// copy (right now) from a stored change (still wrong from PSRAM). Only a line that lies wholly inside the buffer is
// synced: the single-file path's buffer is a plain malloc, and a line shared with a neighbouring block is left alone.
static void log_flip(flip_t const *fl, uint8_t const *data, size_t len) {
    uint8_t const *at = data + fl->off;
    uintptr_t      line;
    if (!ota_line_inside((uintptr_t)data, len, (uintptr_t)at, CACHE_LINE, &line)) {
        ESP_LOGW(TAG, "flip at file offset 0x%zx, psram %p: file 0x%02x, staging 0x%02x (msync skipped: line not inside "
                      "the buffer)", fl->off, (void const *)at, fl->in_file, fl->staging);
        return;
    }
    esp_err_t err = esp_cache_msync((void *)line, CACHE_LINE,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    uint8_t   inv = peek(at);
    ESP_LOGW(TAG, "flip at file offset 0x%zx, psram %p: file 0x%02x, staging 0x%02x, after write-back+invalidate 0x%02x (%s)",
             fl->off, (void const *)at, fl->in_file, fl->staging, inv, err == ESP_OK ? "msync ok" : esp_err_to_name(err));
}

// Up to DIFF_LOG_MAX bytes where the temp file differs from the staging buffer, with plain reads only
static size_t collect_flips(FILE *f, uint8_t const *data, size_t len, uint8_t *bounce, size_t cap, flip_t *out) {
    size_t n_out = 0;
    for (size_t at = 0; at < len && n_out < DIFF_LOG_MAX; at += cap) {
        size_t n = len - at < cap ? len - at : cap;
        if (file_read(f, at, bounce, n) != 0) {
            break;
        }
        for (size_t i = 0; i < n && n_out < DIFF_LOG_MAX; i++) {
            uint8_t now = peek(data + at + i);
            if (bounce[i] != now) {
                out[n_out++] = (flip_t){.off = at + i, .in_file = bounce[i], .staging = now};
            }
        }
    }
    return n_out;
}

// Whether the staging buffer still hashes to the whole file's SHA-256, taken before anything else touches it
static bool staging_hashes(uint8_t const *data, size_t len, char const *sha_hex) {
    uint8_t      digest[SHA256_DIGEST_LEN];
    char         hex[OTA_SHA256_HEX + 1];
    sha256_ctx_t sha;
    sha256_init(&sha);
    sha256_update(&sha, data, len);
    sha256_final(&sha, digest);
    ota_hex(digest, sizeof(digest), hex);
    return strcmp(hex, sha_hex) == 0;
}

// The diagnosis after a mismatch: the staging hash first, then the differing bytes by plain reads, then the probes,
// so no msync taints a read that comes after it
__attribute__((noinline)) static void log_flips(char const *temp, FILE *f, uint8_t const *data, size_t len, uint8_t *bounce,
                                                size_t cap, char const *sha_hex) {
    bool   same = staging_hashes(data, len, sha_hex);
    size_t bad  = ota_first_diff_block(file_read, f, data, len, bounce, cap, 4096);
    char   block[24] = "none";
    if (bad != (size_t)-1) {
        snprintf(block, sizeof(block), "at %zu", bad);
    }
    flip_t flips[DIFF_LOG_MAX];
    size_t n = collect_flips(f, data, len, bounce, cap, flips);
    ESP_LOGW(TAG, "read-back of %s differs, first differing 4 KB block %s; staging buffer %p hashes %s the whole file's "
                  "SHA-256 (%zu differing bytes logged)", temp, block, (void const *)data, same ? "to" : "NOT to", n);
    for (size_t i = 0; i < n; i++) {
        log_flip(&flips[i], data, len);
    }
}

// Read the temp file back and compare its SHA-256 with the whole file's (#593): a bit that flipped between the check
// in PSRAM and the flash is caught here, before the swap, instead of at the next boot. The bounce buffer is internal.
// OTA_PLACE_WRITE (errno set) when the temp can't be opened or read, OTA_PLACE_VERIFY when it differs.
__attribute__((noinline)) static ota_place_t written_ok(char const *temp, uint8_t const *data, size_t len, char const *sha_hex) {
    FILE *f = fopen(temp, "rb");
    if (!f) {
        int e = errno;
        ESP_LOGW(TAG, "read-back of %s: can't open it (%s)", temp, strerror(e));
        errno = e;
        return OTA_PLACE_WRITE;
    }
    uint8_t bounce[FLASH_CHUNK_BOUNCE];
    int     res = ota_readback_check(file_read, f, len, bounce, sizeof(bounce), sha_hex);
    int     e   = errno;
    if (res < 0) {
        ESP_LOGW(TAG, "read-back of %s failed (%s), first differing 4 KB block none", temp, strerror(e));
    } else if (res > 0) {
        log_flips(temp, f, data, len, bounce, sizeof(bounce), sha_hex);
    }
    fclose(f);
    errno = e;
    return res == 0 ? OTA_PLACE_OK : res < 0 ? OTA_PLACE_WRITE : OTA_PLACE_VERIFY;
}

// Room for an upload of size bytes on the drive vms names (#299), else a 507 now instead of a failed write after the
// transfer. A drive whose free space can't be read is let through: the write reports that.
// body: bytes of a request body not read yet (0: none, or the client waits for 100 Continue), dropped before the reply.
bool ota_storage_room(int fd, char const *vms, uint64_t size, size_t pre_len, long long body) {
    uint64_t free_b = 0, total_b;
    bool     sd     = ota_path_is_sd(vms);
    sd_info_t info;
    if (sd) {
        sd_info(&info);
        free_b = info.free_bytes;
    }
    if (sd ? !info.free_known : !storage_free_bytes(&free_b, &total_b)) {
        return true;
    }
    if (ota_file_fits(free_b, size, ota_cluster_bytes(vms))) {
        return true;
    }
    ota_drain_body(fd, pre_len, body);
    ota_respond(fd, 507, "not enough space on the %s: need %llu bytes, %llu free, nothing written",
                sd ? "SD card" : "internal storage", (unsigned long long)size, (unsigned long long)free_b);
    return false;
}

bool ota_drive_ready(int fd, char const *vms) {
    if (!ota_sd_missing(vms, sd_mounted())) {
        return true;
    }
    ota_respond(fd, 409, "no SD card mounted, nothing written");
    return false;
}

// The swap alone (the temp file is written): see place_file. Sets errno for what failed.
ota_place_t ota_swap_temp(char const *path, char const *temp, bool sd) {
    if (rename(temp, path) == 0) {
        return OTA_PLACE_OK;
    }
    if (ota_rename_retry(sd, errno) && unlink(path) == 0) {
        return rename(temp, path) == 0 ? OTA_PLACE_OK : OTA_PLACE_LOST;
    }
    return OTA_PLACE_SWAP;
}

// The write and the swap of one upload, under the card's batch lock on SD0: (a card pulled mid-write fails the call).
// FLASH0: is LittleFS (#300): one rename replaces the old file atomically. SD0: is FAT, whose rename refuses an
// existing target: the old file is removed first, so there a power cut between the two leaves the data in the temp
// file (OTA_PLACE_LOST when the second rename fails: the temp stays). A target another task has open stays put on either.
static ota_place_t place_file(char const *path, char const *temp, bool sd, uint8_t const *data, size_t len, char const *sha_hex,
                            int *err) {
    if (ota_is_directory(path)) { // the early check in the handler ran outside the card's lock
        return OTA_PLACE_DIR;
    }
    ota_mkdir_parents(path);
    unlink(temp); // left by an upload that was cut; only the target's own directory is cleaned
    ota_place_t step = write_file(temp, data, len) ? OTA_PLACE_OK : OTA_PLACE_WRITE;
    if (step == OTA_PLACE_OK) {
        step = written_ok(temp, data, len, sha_hex); // on a failure the old file stays; the temp is dropped
    }
    if (step == OTA_PLACE_OK) {
        step = ota_swap_temp(path, temp, sd);
    }
    *err = errno;
    if (ota_place_drops_temp(step)) {
        unlink(temp);
    }
    return step;
}

// Answer how a file's swap into place ended (ota_finish_file and the pieces' SD0: finish); true = it landed
bool ota_place_reply(int fd, ota_place_t step, int err, ota_place_names_t const *n, size_t len, long long sent) {
    char const *vms = n->vms;
    if (step == OTA_PLACE_DIR) {
        ota_respond(fd, 409, "%s is a directory, nothing written", vms);
        return false;
    }
    if (step == OTA_PLACE_WRITE) {
        ota_respond(fd, 500, "couldn't write %s, file left as it was", n->temp);
        return false;
    }
    if (step == OTA_PLACE_VERIFY) {
        ota_respond(fd, 400, OTA_READBACK_REPLY, vms);
        return false;
    }
    if (step == OTA_PLACE_LOST) {
        ota_respond(fd, 500, "old file removed, new data kept as ota_upload.tmp (%s)", strerror(err));
        return false;
    }
    if (step == OTA_PLACE_SWAP) {
        ota_respond(fd, 500, "couldn't move the upload into place (%s), file left as it was", strerror(err));
        return false;
    }
    ESP_LOGI(TAG, "wrote %s (%zu bytes)", n->path, len);
    sd_file_landed(n->path); // #553: the SD apps' loader rescans a manifest that landed (the card's lock is given by now)
    ota_respond(fd, 200, "wrote %zu bytes (%lld sent) to %s", len, sent, vms);
    return true;
}

// Put a checked file in place: write it next to the target, then swap it in. vms is the path as badge.sh names it.
bool ota_finish_file(int fd, char const *vms, uint8_t const *data, size_t len, long long sent,
                     char const *sha_hex) {
    char path[OTA_PATH_MAX + 16];
    char temp[sizeof(path) + sizeof(OTA_TEMP_NAME)];
    if (!ota_path_to_posix(vms, path, sizeof(path)) || !ota_temp_to_posix(vms, temp, sizeof(temp))) {
        ota_respond(fd, 400, "bad path");
        return false;
    }
    ESP_LOGI(TAG, "file push to %s: staging buffer %p, %zu bytes", vms, (void const *)data, len);
    if (!ota_drive_ready(fd, vms) || !ota_storage_room(fd, vms, len, 0, 0) || !ota_file_change_begin(fd, path)) {
        return false; // before the bundle is marked changed: the old file stays
    }
    // The change stays marked in progress until the rename has landed or failed: "fw onair on" in between must see it
    // (#454). A C6 file's change also stays pending through c6link_bundle_changed.
    bool sd = ota_path_is_sd(vms);
    if (sd && !sd_begin(SD_WAIT_MS)) {
        ota_change_end(false);
        ota_respond(fd, 409, "SD card busy or not mounted, nothing written");
        return false;
    }
    int err  = 0;
    ota_place_t step = place_file(path, temp, sd, data, len, sha_hex, &err);
    if (sd) {
        // A read-back that differs is damage in memory, not the card failing (#593)
        sd_end(step == OTA_PLACE_OK ? SD_IO_OK : step == OTA_PLACE_VERIFY ? SD_IO_NEUTRAL : sd_io_from_errno(err));
    }
    ota_change_end(false);
    ota_place_names_t const names = {.vms = vms, .path = path, .temp = temp};
    return ota_place_reply(fd, step, err, &names, len, sent);
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
        ota_respond(fd, 403, NOT_ALLOWED);
        return;
    }
    if (!ota_drive_ready(fd, req->path)) {
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
    if (!req->deflate && !ota_storage_room(fd, req->path, (uint64_t)req->content_length, pre_len, req->expect_continue ? 0 : req->content_length)) {
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
    hook_end(ota_finish_file(fd, req->path, mem.data, mem.len, ota_last_received, sha_hex), false);
    free(mem.data);
}

static void handle_delete(int fd, ota_request_t const *req) {
    char path[OTA_PATH_MAX + 16];
    if (!req->has_path) {
        ota_respond(fd, 400, "needs X-Path");
        return;
    }
    if (!ota_path_to_posix(req->path, path, sizeof(path))) {
        ota_respond(fd, 403, NOT_ALLOWED);
        return;
    }
    if (!ota_drive_ready(fd, req->path) || !ota_file_change_begin(fd, path)) {
        return;
    }
    bool sd = ota_path_is_sd(req->path);
    if (sd && !sd_begin(SD_WAIT_MS)) {
        ota_change_end(false);
        ota_respond(fd, 409, "SD card busy or not mounted, nothing deleted");
        return;
    }
    int gone = unlink(path);
    int err  = errno;
    if (sd) {
        sd_end(gone == 0 || err == ENOENT ? SD_IO_OK : sd_io_from_errno(err));
    }
    ota_change_end(false);
    if (gone != 0) { // EBUSY: open in another task (esp_littlefs, #300); the file stays (a C6 one's bundle is forgotten)
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
