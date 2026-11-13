// Wi-Fi update server of the lean firmware (#61), adapted from BadgeVMS's OTA receiver app
// (sdk_apps/ota_receiver/main.c): same protocol (ota_proto.h) so tools/badge.sh ota-* works unchanged. It runs
// whenever Wi-Fi is up and Settings › Updates over Wi-Fi is on (#44); the token guards every route. Bodies are
// received into memory and checked before anything touches flash: flash writes while the network is receiving corrupted the stream on this badge (#40).
#include "secure_wipe.h"
#include "ota_server.h"

#include "esp_attr.h"

#include "c6_paths.h"
#include "c6link.h"
#include "lichen_mdns.h"
#include "wifi_service.h"
#include "console_cmd.h" // CONSOLE_WIFI_CMD_MAX
#include "fw_console.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "assets.h"
#include "assets_fmt.h"
#include "esp_ota_ops.h"
#include "flash_chunk.h"
#include "esp_partition.h"
#include "lichen_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_autooff.h"
#include "ota_bmp.h"
#include "ota_hw.h"
#include "ota_inflate.h"
#include "ota_path.h"
#include "ota_pieces.h"
#include "ota_proto.h"
#include "ota_rle.h"
#include "ota_trial.h"
#include "ota_valid.h"
#include "miniz.h" // the ROM inflater
#include "ota_os.h"
#include "sha256.h"
#include "storage.h"

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

#define TOKEN_FILE      STORAGE_BASE "/ota_token.txt"
#define FIRMWARE_MAX    (4 * 1024 * 1024) // one app slot (firmware/partitions.csv)
#define FILE_MAX        (4 * 1024 * 1024) // received into PSRAM before it touches storage (#40)
#define ASSETS_MAX      (3 * 1024 * 1024) // the assets partition (firmware/partitions.csv, #22)
#define READ_TIMEOUT_S  10
#define HEAD_DEADLINE_S 15
#define BODY_MIN_RATE   (16 * 1024) // bytes/s a body must average, on top of BODY_GRACE_S
#define BODY_GRACE_S    30
#define SHOT_SLICE      (16 * 1024) // bytes of pixels compressed and sent per round of a screenshot
#define SHOT_SEND_S     4   // one send of a screenshot (about 10x fewer bytes than raw, so a dead link frees the task fast)
#define SHOT_TOTAL_S    10  // whole screenshot send (#146)
#define BODY_SEND_S     10
#define LOG_SLICE       2048  // bytes copied out of the log store per call (interrupts are off meanwhile)
#define CRASHES_REPLY_MAX (12 * 1024) // 20 records of 256 bytes and the header
#define LOG_REPLY_MAX   (24 * 1024) // one GET /log answer; the client asks again from X-Next-Seq
#define CONSOLE_REPLY   4096
#define BODY_TOTAL_S    45 // whole send: a client that drips bytes must not hold the one server task
// Flash writes happen on this task, so its stack must be in internal RAM; peaks measured in BadgeVMS: 7 KB
#define SERVER_STACK    16384
// The screenshot worker only compresses and sends from PSRAM, so a small internal stack does (#146)
#define SHOT_STACK      4096

static char const TAG[] = "ota";

static char token[OTA_TOKEN_LEN + 1];
static bool token_refused; // no token kept anywhere this boot (#116): the server never listens

// Settings › Updates over Wi-Fi (#44). On by default while Lichen is in development: tools/badge.sh, the Wi-Fi console,
// the log and screenshots all go through this server. Off closes the port; the USB console (`fw ota on`) reopens it.
#define OTA_WIFI_KEY     "ota_wifi"
#define OTA_WIFI_DEFAULT 1
static volatile bool server_on;
// Optional auto-off (#110, ota_autooff.h): minutes after boot or the last switch-on, 0 (the default) never. A close by
// the timer lasts this boot only and leaves the saved setting on.
#define OTA_AUTO_KEY "ota_auto_min"
static volatile uint8_t  auto_min;
static volatile uint32_t armed_s; // monotonic seconds at boot (0) or at the last switch-on
static volatile bool     auto_closed;

static long now_s(void);

bool ota_server_enabled(void) {
    return ota_server_listens(server_on, !token_refused);
}

bool ota_server_refused(void) {
    return token_refused;
}

void ota_server_set_enabled(bool on) {
    if (on) {
        armed_s = (uint32_t)now_s(); // the auto-off counts from now
    }
    auto_closed = false; // on or off by hand supersedes a timer close: the saved setting now says what it says (#125)
    server_on = on;
    ota_os_set_u8(OTA_WIFI_KEY, on ? 1 : 0);
    ESP_LOGI(TAG, "update server over Wi-Fi switched %s", on ? "on" : "off");
}

uint8_t ota_server_auto_off(void) {
    return auto_min;
}

void ota_server_set_auto_off(uint8_t minutes) {
    armed_s  = (uint32_t)now_s(); // a new time counts from now, not from boot
    auto_min = minutes;
    ota_os_set_u8(OTA_AUTO_KEY, minutes);
    ESP_LOGI(TAG, "update server auto-off: %u min (0 = never)", (unsigned)minutes);
}

bool ota_server_auto_closed(void) {
    return auto_closed;
}

// One request at a time
// PSRAM (#107): sockets copy, no DMA. Flash is never read into body_buf: esp_flash_read would malloc an internal temp
// buffer as big as the read for it; slot and partition reads go through a FLASH_CHUNK_BOUNCE stack buffer (#364).
EXT_RAM_BSS_ATTR static char    head_buf[2048];
EXT_RAM_BSS_ATTR static uint8_t body_buf[4096];

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

// The reply head and a body (NUL terminated, ends in a newline), sent as is
static void send_reply(int fd, int status, char const *body) {
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
        case 507: reason = "Insufficient Storage"; break;
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

static void respond(int fd, int status, char const *fmt, ...) {
    char    body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    strcat(body, "\n");
    send_reply(fd, status, body);
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

static bool output_bytes(void *ctx, uint8_t const *data, size_t len) {
    output_t *out = ctx;
    if (last_decoded + (long long)len > out->max) {
        return false;
    }
    sha256_update(&out->sha, data, len);
    last_decoded += (long long)len;
    return out->sink(out->ctx, data, len);
}

// Streaming inflate of a zlib body with the ROM's tinfl; its 32 KiB dictionary doubles as the output window. The rules
// a stream from the network must keep (output cap, nothing after the end, progress) are ota_inflate.c's (#298).
EXT_RAM_BSS_ATTR static tinfl_decompressor inflater; // 43 KB with the window: PSRAM (#107)
EXT_RAM_BSS_ATTR static uint8_t            inflate_window[TINFL_LZ_DICT_SIZE];
static size_t             inflate_ofs;
static bool               inflate_more; // the last step left output: the next one runs without a socket read between

static ota_inflate_status_t tinfl_step(void *dec, uint8_t const *in, size_t *in_len, uint8_t const **out,
                                       size_t *out_len) {
    (void)dec;
    if (inflate_more) {
        // Back-to-back windows (32 KiB decoded, then written to flash) would otherwise hold core 0 for a whole piece;
        // one tick lets the idle task (and the task watchdog) run between them (#298)
        vTaskDelay(1);
    }
    size_t       space  = sizeof(inflate_window) - inflate_ofs;
    tinfl_status status = tinfl_decompress(&inflater, in, in_len, inflate_window, inflate_window + inflate_ofs, &space,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
    *out        = inflate_window + inflate_ofs;
    *out_len    = space;
    inflate_ofs  = (inflate_ofs + space) & (sizeof(inflate_window) - 1);
    inflate_more = status == TINFL_STATUS_HAS_MORE_OUTPUT;
    if (status < TINFL_STATUS_DONE) {
        return OTA_INFLATE_ERROR;
    }
    return status == TINFL_STATUS_DONE               ? OTA_INFLATE_DONE
           : status == TINFL_STATUS_HAS_MORE_OUTPUT ? OTA_INFLATE_HAS_OUTPUT
                                                     : OTA_INFLATE_NEEDS_INPUT;
}

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

// One read of the body: what sits behind the head first, then the socket within the body's deadline
static ssize_t body_chunk(int fd, body_t *b, long long got, long deadline, uint8_t const **chunk) {
    if (b->pre_len > 0) {
        size_t n   = b->pre_len;
        *chunk     = b->pre;
        b->pre_len = 0;
        return (ssize_t)n;
    }
    if (now_s() > deadline) {
        return -1;
    }
    size_t want = b->total - got < (long long)sizeof(body_buf) ? (size_t)(b->total - got) : sizeof(body_buf);
    *chunk      = body_buf;
    return read(fd, body_buf, want);
}

// Read the body, decode it if it is compressed, hash it and feed it to its sink. sha_hex is the SHA-256 of the decoded
// bytes, i.e. of the file as it ends up. A compressed body must end its stream.
static bool receive_body(int fd, body_t b, char sha_hex[OTA_SHA256_HEX + 1]) {
    static output_t      out; // static, not on the stack: one transfer at a time
    EXT_RAM_BSS_ATTR static ota_inflate_t z; // PSRAM like the inflater: this task only, no DMA
    sha256_init(&out.sha);
    out.sink = b.sink;
    out.ctx  = b.ctx;
    out.max  = b.max;
    if (b.deflate) {
        tinfl_init(&inflater);
        inflate_ofs  = 0;
        inflate_more = false;
        ota_inflate_begin(&z, tinfl_step, NULL, b.max > 0 ? (size_t)b.max : 0);
    }

    // A per-read timeout alone lets a client trickle bytes forever; the whole body gets a deadline too
    long      deadline = now_s() + BODY_GRACE_S + (long)(b.total / BODY_MIN_RATE);
    long long got      = 0;
    last_received      = 0;
    last_decoded       = 0;
    hook_progress(progress_base);
    if (b.pre_len > (size_t)b.total) {
        b.pre_len = (size_t)b.total;
    }
    while (got < b.total || b.pre_len > 0) {
        uint8_t const *chunk;
        ssize_t        n = body_chunk(fd, &b, got, deadline, &chunk);
        if (n <= 0) {
            return false;
        }
        if (!(b.deflate ? ota_inflate_feed(&z, chunk, (size_t)n, output_bytes, &out)
                        : output_bytes(&out, chunk, (size_t)n))) {
            return false;
        }
        got           += n;
        last_received  = got;
        hook_progress(progress_base + (b.piece ? last_decoded : got));
    }
    // The stream must have ended; a piece must decode to exactly its X-Piece-Size (b.max), a whole body to anything
    // up to its cap
    if (b.deflate && !(b.piece ? ota_inflate_complete(&z, (size_t)b.max) : ota_inflate_complete(&z, z.produced))) {
        return false;
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

// A change under the C6 image directory makes the stored "verified bundle" stale (#36); forget it before touching the file,
// so even an interrupted write leaves the next boot with a full check. Every writer of the C6 image directory must go
// through here (or call c6link_bundle_changed itself): the skip key only covers sizes and .md5 text
static void forget_c6_bundle_if_c6_file(char const *posix_path) {
    if (strncmp(posix_path, C6_IMAGE_DIR "/", strlen(C6_IMAGE_DIR) + 1) == 0) {
        c6link_bundle_changed();
    }
}

// Room for an upload of size bytes on /storage (#299), else a 507 now instead of a failed write after the transfer.
// A drive whose free space can't be read is let through: the write reports that.
// body: bytes of a request body not read yet (0: none, or the client waits for 100 Continue), dropped before the reply.
static void drain_body(int fd, size_t pre_len, long long content_length);
static bool storage_room(int fd, uint64_t size, size_t pre_len, long long body) {
    uint64_t free_b, total_b;
    if (!storage_free_bytes(&free_b, &total_b) || ota_file_fits(free_b, size, STORAGE_CLUSTER)) {
        return true;
    }
    drain_body(fd, pre_len, body);
    respond(fd, 507, "not enough space on the internal storage: need %llu bytes, %llu free, nothing written",
            (unsigned long long)size, (unsigned long long)free_b);
    return false;
}

// Read and drop a refused body (at most REFUSE_DRAIN_MAX bytes, within REFUSE_DRAIN_S) so the reply isn't lost to the
// reset a close with unread data causes; a larger body is left: badge.sh checks the space before it sends (#299)
#define REFUSE_DRAIN_MAX (256 * 1024)
#define REFUSE_DRAIN_S   5
static void drain_body(int fd, size_t pre_len, long long content_length) {
    if (content_length <= 0 || content_length > REFUSE_DRAIN_MAX || (long long)pre_len >= content_length) {
        return;
    }
    long long left = content_length - (long long)pre_len;
    long      stop = now_s() + REFUSE_DRAIN_S;
    uint8_t   scratch[256];
    set_read_timeout(fd, 1, 0);
    while (left > 0 && now_s() < stop) {
        ssize_t n = read(fd, scratch, left < (long long)sizeof(scratch) ? (size_t)left : sizeof(scratch));
        if (n <= 0) {
            break;
        }
        left -= n;
    }
}

// Put a checked file in place: write it next to the target, then swap it in. vms is the path as badge.sh names it.
static bool finish_file(int fd, char const *vms, uint8_t const *data, size_t len, long long sent) {
    char path[OTA_PATH_MAX + 16];
    char temp[sizeof(path) + sizeof(OTA_TEMP_NAME)];
    if (!ota_path_to_posix(vms, path, sizeof(path)) || !ota_temp_to_posix(vms, temp, sizeof(temp))) {
        respond(fd, 400, "bad path");
        return false;
    }
    if (!storage_room(fd, len, 0, 0)) { // before the bundle is marked changed: the old file stays
        return false;
    }
    forget_c6_bundle_if_c6_file(path);
    mkdir_parents(path);
    if (!write_file(temp, data, len)) {
        unlink(temp);
        respond(fd, 500, "couldn't write %s, file left as it was", temp);
        return false;
    }
    // FLASH0: is /storage, LittleFS since #300: one rename replaces the old file atomically (a cut leaves the old file
    // or the new one). A target another task has open stays put: esp_littlefs refuses the rename with EBUSY.
    if (rename(temp, path) != 0) {
        int err = errno;
        unlink(temp);
        respond(fd, 500, "couldn't move the upload into place (%s), file left as it was", strerror(err));
        return false;
    }
    ESP_LOGI(TAG, "wrote %s (%zu bytes)", path, len);
    respond(fd, 200, "wrote %zu bytes (%lld sent) to %s", len, sent, vms);
    return true;
}

// Validate what was written to the slot and make it the boot slot; it runs after a reboot, on trial until it proves itself
static bool install_slot(int fd, esp_partition_t const *slot, esp_ota_handle_t handle, size_t len, long long sent) {
    esp_err_t err = esp_ota_end(handle); // checks the image: chip, segments, hash
    if (err != ESP_OK) {
        respond(fd, 422, "image failed validation (%s), nothing installed", esp_err_to_name(err));
        return false;
    }
    if ((err = esp_ota_set_boot_partition(slot)) != ESP_OK) {
        respond(fd, 500, "couldn't select %s (%s)", slot->label, esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "firmware installed in %s (%zu bytes)", slot->label, len);
    ota_image_note_wifi_install(slot); // #411: its trial needs Wi-Fi back (ota_trial.c)
    respond(fd, 200, "installed %zu bytes (%lld sent), runs after a reboot", len, sent);
    firmware_installed = true;
    return true;
}

// The valid mark also waits for a live PSRAM trial (60 s), so "45 s" (OTA_VALID_AFTER_S) undersells it (#302)
#define TRIAL_REFUSAL                                                                                                  \
    "the running image is still on trial: try again once 'fw slot' shows it valid (about a minute after boot); "        \
    "nothing was stopped"

// esp_ota_begin refused: say why when the reason is the running image still being on trial (#5)
static void respond_begin_failed(int fd, esp_err_t err) {
    if (ota_image_trial_pending()) {
        respond(fd, 503, "%s", TRIAL_REFUSAL);
    } else {
        respond(fd, 503, "couldn't start an OTA session (%s)", esp_err_to_name(err));
    }
}

// Install a checked firmware image (received whole in memory) into the other app slot
static bool finish_firmware(int fd, uint8_t const *data, size_t len, long long sent) {
    if (!ota_image_header_ok(data, len, CONFIG_IDF_FIRMWARE_CHIP_ID)) {
        respond(fd, 422, "not an app image for this chip, nothing erased or installed");
        return false;
    }
    esp_partition_t const *slot = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t       handle;
    esp_err_t              err  = slot ? esp_ota_begin(slot, len, &handle) : ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) {
        respond_begin_failed(fd, err);
        return false;
    }
    if (esp_ota_write(handle, data, len) != ESP_OK) {
        esp_ota_abort(handle);
        respond(fd, 500, "flash write failed, nothing installed");
        return false;
    }
    return install_slot(fd, slot, handle, len, sent);
}

// A partition read for ota_hash_range; buf is always an internal bounce buffer (#364)
static int slot_read(void *ctx, size_t off, uint8_t *buf, size_t n) {
    return esp_partition_read(ctx, off, buf, n) == ESP_OK ? 0 : -1;
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
__attribute__((noinline)) static bool finish_assets(int fd, uint8_t const *data, size_t len, long long sent) {
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
    if (err == ESP_OK) {
        uint8_t     bounce[FLASH_CHUNK_BOUNCE];
        part_read_t r    = {.part = part, .err = ESP_OK};
        int         same = flash_chunk_same(part_read, &r, 0, data, len, bounce, sizeof(bounce));
        err              = same == 1 ? ESP_OK : same == 0 ? ESP_ERR_INVALID_CRC : r.err != ESP_OK ? r.err : ESP_FAIL;
    }
    if (err != ESP_OK) {
        respond(fd, 500, "writing the assets failed (%s); the built-in fonts are used until it succeeds",
                esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "assets written (%zu bytes, %u files)", len, (unsigned)view.count);
    respond(fd, 200, "wrote %zu bytes (%lld sent), %u files, used after a reboot", len, sent, (unsigned)view.count);
    return true;
}


static void handle_firmware(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    if (!ota_whole_body_ok(req)) {
        respond(fd, 400, "X-Piece-Size belongs to a piece (with X-Offset), nothing installed");
        return;
    }
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
    body_t body = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                   .max = FIRMWARE_MAX, .sink = memory_sink, .ctx = &mem};
    bool   ok   = receive_body(fd, body, sha_hex);
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
    // rebooting: the badge restarts into it by itself, unless the sender holds that (X-Reboot: hold, #298)
    bool installed = finish_firmware(fd, mem.data, mem.len, last_received);
    hook_end(installed, installed && !req->hold_reboot);
    free(mem.data);
}

// A push onto a directory name (e.g. FLASH0:c6) must not replace the directory (#5)
static bool is_directory(char const *posix_path) {
    struct stat st;
    return stat(posix_path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void handle_file(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    char path[OTA_PATH_MAX + 16];
    if (!file_allowed()) {
        respond(fd, 409, "updating");
        return;
    }
    if (!ota_whole_body_ok(req)) {
        respond(fd, 400, "X-Piece-Size belongs to a piece (with X-Offset), file left as it was");
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
    if (is_directory(path)) {
        respond(fd, 409, "%s is a directory, nothing written", req->path);
        return;
    }
    // A compressed body's size is known once it is in: finish_file checks that one
    if (!req->deflate && !storage_room(fd, (uint64_t)req->content_length, pre_len, req->expect_continue ? 0 : req->content_length)) {
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
    body_t body = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                   .max = FILE_MAX, .sink = memory_sink, .ctx = &mem};
    bool   ok   = receive_body(fd, body, sha_hex);
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
    ota_pieces_t pieces; // SHA-256 of each verified piece, to find damaged ones if the whole file doesn't match
    // Firmware pieces are not staged in memory (#37): each goes straight into the inactive slot, which is read back
    // and hashed at the end. data stays NULL then.
    esp_partition_t const *slot;
    esp_ota_handle_t       ota;
    bool                   ota_open;
    bool                   dirty; // the last attempt at the current piece wrote (part of) it: erase before the next
    long                   last_s; // when the last piece came: an upload left half-way is dropped (piece_expire)
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

static void piece_expire(bool now) {
    if (staged.active && (now || now_s() - staged.last_s > PIECE_IDLE_S)) {
        ESP_LOGW(TAG, "upload in pieces abandoned at %zu of %zu bytes: dropped", staged.got, staged.total);
        hook_end(false, false);
        staged_reset();
    }
}

// Closes the server when its auto-off time has passed (#110); never during an upload in pieces. Runs about once a
// second on this task, between connections (one running upload is never cut).
static void autooff_check(void) {
    if (server_on && ota_autooff_due(auto_min, armed_s, (uint32_t)now_s(), staged.active)) {
        server_on   = false; // this boot only: the saved setting stays on
        auto_closed = true;
        ESP_LOGI(
            TAG,
            "update server closed by its %u min auto-off (Settings or \"fw ota on\" opens it again)",
            (unsigned)auto_min
        );
    }
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
// noinline, like rehash_piece and finish_assets: each has a bounce buffer, which would cross the 1 KB frame line when
// folded into handle_piece
__attribute__((noinline)) static bool finish_streamed(int fd, char sha_hex[OTA_SHA256_HEX + 1]) {
    uint8_t digest[SHA256_DIGEST_LEN];
    uint8_t bounce[FLASH_CHUNK_BOUNCE]; // internal, on the server's stack: not body_buf (#364)
    if (ota_hash_range(slot_read, (void *)staged.slot, 0, staged.total, bounce, sizeof(bounce), digest) != 0) {
        respond(fd, 500, "couldn't read the slot back, nothing installed");
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
        respond(fd, 400, "whole-file SHA-256 mismatch (sha256 %s), nothing installed", sha_hex);
        return false;
    }
    staged.ota_open = false; // esp_ota_end ends the handle, good or bad
    return install_slot(fd, staged.slot, staged.ota, staged.total, staged.sent);
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
    uint8_t bounce[FLASH_CHUNK_BOUNCE]; // internal, on the server's stack: not body_buf (#364)
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
        !storage_room(fd, (uint64_t)req->total_size, *pre_len, req->expect_continue ? 0 : req->content_length)) {
        return false;
    }
    if (!stream) {
        size_t cap  = ota_round_up((size_t)req->total_size, STAGE_ALIGN); // 0 only on overflow: never allocated
        staged.data = cap ? heap_caps_aligned_alloc(STAGE_ALIGN, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!staged.data) {
            respond(fd, 503, "not enough memory for %lld bytes", req->total_size);
            return false;
        }
    }
    staged.active = true;
    hook_begin(req->route == OTA_ROUTE_FIRMWARE ? OTA_KIND_FIRMWARE
               : req->route == OTA_ROUTE_ASSETS ? OTA_KIND_ASSETS
                                                : OTA_KIND_FILE,
               req->route == OTA_ROUTE_FILE     ? display_name(req->path)
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
            send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
            *continued = true;
        }
        // Offset 0 of firmware is never compressed (ota_piece_headers), so these are the image's own first bytes
        size_t want = req->content_length < OTA_IMAGE_HEADER_LEN ? (size_t)req->content_length : OTA_IMAGE_HEADER_LEN;
        if (!peek_body(fd, pre, pre_len, want) || !ota_image_header_ok(*pre, *pre_len, CONFIG_IDF_FIRMWARE_CHIP_ID)) {
            hook_end(false, false);
            staged_reset();
            respond(fd, 422, "not an app image for this chip, nothing erased");
            return false;
        }
        esp_err_t err = stream_open((size_t)req->content_length);
        if (err != ESP_OK) {
            hook_end(false, false);
            staged_reset();
            respond_begin_failed(fd, err);
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
        respond(fd, 400, "whole-file SHA-256 mismatch (sha256 %s), nothing written", sha_hex);
        return;
    }
    finished = staged.route == OTA_ROUTE_ASSETS ? finish_assets(fd, staged.data, staged.total, staged.sent)
                                                : finish_file(fd, staged.path, staged.data, staged.total, staged.sent);
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
        respond(fd, 500, "couldn't erase the slot for the piece at %lld, send it again", req->offset);
        return false;
    }
    staged.dirty = stream; // from here on the slot holds (part of) this piece until it is verified
    if (req->expect_continue && !continued) {
        send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    }
    progress_base = (uint32_t)staged.got;
    body_t body   = {.pre = pre, .pre_len = pre_len, .total = req->content_length, .deflate = req->deflate,
                     .max = (long long)len, .sink = stream ? flash_sink : fixed_sink,
                     .ctx = stream ? (void *)&flash : (void *)&mem, .piece = true};
    bool   ok     = receive_body(fd, body, sha_hex); // a compressed piece: exactly len bytes (ota_inflate_complete)
    if (!ok || strcmp(sha_hex, req->sha256) != 0) {
        respond(fd, 400, "piece at %lld %s, send it again", req->offset, ok ? "arrived damaged" : "didn't arrive");
        return false; // staged.got is unchanged, so the same piece can come again
    }
    // The piece was hashed on its way through the internal buffer; hash it again where it landed (#37)
    uint8_t stored[SHA256_DIGEST_LEN];
    rehash_piece(staged.got, len, stored);
    ota_hex(stored, sizeof(stored), sha_hex);
    if (strcmp(sha_hex, req->sha256) != 0) {
        ESP_LOGW(TAG, "piece at %lld damaged in %s, asking for it again", req->offset, stream ? "the slot" : "PSRAM");
        respond(fd, 400, "piece at %lld was damaged in the badge's memory, send it again", req->offset);
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
            respond(fd, 400, "a piece needs Content-Length, X-Content-SHA256, X-Total-Size and X-File-SHA256");
            return true;
        case OTA_PIECE_BAD_DEFLATE:
            respond(fd, 400, "a compressed piece needs X-Piece-Size (its decoded bytes), and only a compressed piece has one");
            return true;
        case OTA_PIECE_FIRST_PLAIN:
            respond(fd, 400, "the first firmware piece comes uncompressed: its image header is checked before anything is erased");
            return true;
        case OTA_PIECE_BLOATED: respond(fd, 413, "compressed piece larger than its X-Piece-Size allows"); return true;
        case OTA_PIECE_ZTOO_LARGE:
            respond(fd, 413, "a compressed piece decodes to at most %d bytes", OTA_ZPIECE_MAX);
            return true;
        case OTA_PIECE_TOO_LARGE: respond(fd, 413, "larger than %lld bytes", max); return true;
        case OTA_PIECE_UNALIGNED:
            respond(fd, 400, "firmware pieces start on a 4096-byte boundary and are whole multiples of it, except the last");
            return true;
        case OTA_PIECE_PAST_END: respond(fd, 400, "piece runs past X-Total-Size"); return true;
        default: return false;
    }
}

// Every check a piece passes before anything is touched, answered in the order they always were; true = refused.
// The bounds come last but before piece_start() shows the update screen (#125).
static bool piece_refused(int fd, ota_request_t const *req, char *posix, size_t posix_cap) {
    long long max = req->route == OTA_ROUTE_FIRMWARE ? FIRMWARE_MAX : req->route == OTA_ROUTE_ASSETS ? ASSETS_MAX : FILE_MAX;
    bool      file = req->route == OTA_ROUTE_FILE;
    if (file && !file_allowed()) { // firmware and assets come after /prepare
        respond(fd, 409, "updating");
        return true;
    }
    if (piece_verdict_sent(fd, ota_piece_headers(req, max, 4096), max)) {
        return true;
    }
    if (file && (!req->has_path || !ota_path_to_posix(req->path, posix, posix_cap))) {
        respond(fd, 403, "path not allowed: use FLASH0: with a plain file name");
        return true;
    }
    if (file && is_directory(posix)) {
        respond(fd, 409, "%s is a directory, nothing written", req->path);
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
        respond(fd, 409, "expected the piece at offset %zu", staged.active ? staged.got : (size_t)0);
        return;
    }
    staged.last_s = now_s();
    if (!piece_receive(fd, req, pre, pre_len, continued)) {
        return;
    }
    if (staged.got < staged.total) {
        respond(fd, 200, "piece ok, %zu of %zu bytes", staged.got, staged.total);
        return;
    }
    piece_done(fd, req->hold_reboot);
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
    if (unlink(path) != 0) { // EBUSY: open in another task (esp_littlefs, #300); the file and its bundle stay
        int err = errno;
        respond(fd, err == ENOENT ? 404 : 409, "couldn't delete %s: %s", req->path, strerror(err));
        return;
    }
    forget_c6_bundle_if_c6_file(path);
    respond(fd, 200, "deleted %s", req->path);
}

// The body: the BMP header, then the pixels as ota_rle packets, compressed slice by slice into one small PSRAM buffer
// (a UI frame is mostly flat colour: about 10:1). There is no Content-Length (the size isn't known up front), so the
// client checks the pixel count; a cut connection leaves it short.
static void send_shot(int fd, uint8_t const *pixels, size_t total, uint8_t const *head) {
    uint8_t *out = heap_caps_malloc(ota_rle_bound(SHOT_SLICE / 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        respond(fd, 503, "out of memory, try again");
        return;
    }
    struct timeval tv = {.tv_sec = SHOT_SEND_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    static char const reply[] = "HTTP/1.1 200 OK\r\nContent-Type: application/x-lichen-bmp-rle\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    bool ok   = send_all(fd, reply, sizeof(reply) - 1) && send_all(fd, head, OTA_BMP_HEADER_LEN);
    long stop = now_s() + SHOT_TOTAL_S;
    for (size_t off = 0; ok && off < total; off += SHOT_SLICE) {
        size_t n = total - off < SHOT_SLICE ? total - off : SHOT_SLICE;
        ok       = now_s() <= stop && send_all(fd, out, ota_rle_encode(pixels + off, n / 2, out));
    }
    free(out);
}

typedef struct {
    int            fd;
    uint8_t const *pixels;
    size_t         total; // bytes of pixels
    uint8_t        head[OTA_BMP_HEADER_LEN];
} shot_t;

// GET /screenshot (#30), first half, on the server task (its big stack runs the LVGL render inside the hook; a small
// task would overflow). Answers the error itself and returns false; true means the picture is held and must be sent
// and freed by shot_send. It is never logged: it can show private messages.
static bool shot_render(int fd, shot_t *shot) {
    if (!hooks.screenshot) {
        respond(fd, 404, "no screen here");
        return false;
    }
    if (!file_allowed()) {
        respond(fd, 409, "a firmware update is running");
        return false;
    }
    uint8_t const *pixels;
    uint32_t       w, h, stride;
    int            status = hooks.screenshot(&pixels, &w, &h, &stride);
    if (status) {
        respond(fd, status, status == 503 ? "the screen is busy, try again" : "no screenshot");
        return false;
    }
    size_t row  = (size_t)w * 2;
    size_t size = ota_bmp565_file_size(w, h);
    // Rows go out as they sit in memory: only a tightly packed picture with 4-byte rows is supported
    if (!size || stride != row || ota_bmp565_row_bytes(w) != row || !ota_bmp565_header(w, h, shot->head)) {
        respond(fd, 500, "unsupported screen shape");
        if (hooks.screenshot_free) {
            hooks.screenshot_free();
        }
        return false;
    }
    shot->fd     = fd;
    shot->pixels = pixels;
    shot->total  = row * h;
    return true;
}

// A reply with a body that is not a short text: head then body in slices, within one total deadline
static void send_body(int fd, char const *head, size_t head_len, char const *body, size_t len) {
    struct timeval tv = {.tv_sec = BODY_SEND_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    bool ok   = send_all(fd, head, head_len);
    long stop = now_s() + BODY_TOTAL_S;
    for (size_t off = 0; ok && off < len; off += SHOT_SLICE) {
        ok = now_s() <= stop && send_all(fd, body + off, len - off < SHOT_SLICE ? len - off : SHOT_SLICE);
    }
}

// GET /log (#38): lines of the log store from ?since= on. At most LOG_REPLY_MAX bytes are collected first (copied out of
// the store in small locked slices), so the headers can carry the exact length and the next sequence number. Lines
// are private (node names, messages): never logged by this server, token required like the rest.
static void handle_log(int fd, ota_request_t const *req) {
    if (!hooks.log_read) {
        respond(fd, 404, "no log here");
        return;
    }
    char *reply = heap_caps_malloc(LOG_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        respond(fd, 503, "out of memory, try again");
        return;
    }
    uint32_t since  = req->since > UINT32_MAX ? UINT32_MAX : (uint32_t)req->since;
    uint32_t next   = since;
    uint32_t missed = 0;
    size_t   total  = 0;
    while (total + LOG_SLICE <= LOG_REPLY_MAX) {
        uint32_t step_missed;
        size_t   n = hooks.log_read(since, (char *)body_buf, LOG_SLICE, &next, &step_missed);
        missed    += step_missed;
        if (n == 0) {
            break;
        }
        memcpy(reply + total, body_buf, n);
        total += n;
        since  = next;
    }
    char head[224];
    int  n = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\nX-Next-Seq: %u\r\n"
                      "X-Log-Missed: %u\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                      (unsigned)total, (unsigned)next, (unsigned)missed);
    send_body(fd, head, (size_t)n, reply, total);
    free(reply);
}

// GET /crashes (#170): the stored crash records as text. Private enough (task names, addresses): token required, never
// logged here.
static void handle_crashes(int fd) {
    if (!hooks.crashes_read) {
        respond(fd, 404, "no crash export here");
        return;
    }
    char *reply = heap_caps_malloc(CRASHES_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        respond(fd, 503, "out of memory, try again");
        return;
    }
    size_t total = hooks.crashes_read(reply, CRASHES_REPLY_MAX);
    if (total == 0 || total > CRASHES_REPLY_MAX) {
        respond(fd, 503, "the crash log could not be read, try again");
        free(reply);
        return;
    }
    char head[160];
    int  n = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      (unsigned)total);
    send_body(fd, head, (size_t)n, reply, total);
    free(reply);
}

typedef struct {
    int fd;
} console_reply_ctx_t;

static void console_done(void *ctx, char const *out, size_t len) {
    char head[160];
    int  n = snprintf(head, sizeof(head),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      (unsigned)len);
    send_body(((console_reply_ctx_t *)ctx)->fd, head, (size_t)n, out, len);
}

// The command line comes in the body (never logged, it can hold a Wi-Fi password): printable ASCII only
static bool read_console_line(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len, char *line) {
    size_t want = (size_t)req->content_length;
    size_t have = pre_len < want ? pre_len : want;
    memcpy(line, pre, have);
    long deadline = now_s() + HEAD_DEADLINE_S;
    set_read_timeout(fd, READ_TIMEOUT_S, 0);
    while (have < want) {
        ssize_t n = read(fd, line + have, want - have);
        if (n <= 0 || now_s() > deadline) {
            return false;
        }
        have += (size_t)n;
    }
    line[have] = '\0';
    for (size_t i = 0; i < have; i++) {
        if ((unsigned char)line[i] < 0x20 || (unsigned char)line[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

// POST /console (#38): one fw command, run through the same dispatcher as the UART console
static void handle_console(int fd, ota_request_t const *req, uint8_t const *pre, size_t pre_len) {
    if (!hooks.console_run) {
        respond(fd, 404, "the Wi-Fi console is off");
        return;
    }
    if (!file_allowed()) {
        respond(fd, 409, "a firmware update is running");
        return;
    }
    if (!console_wifi_len_ok(req->content_length)) {
        respond(fd, req->content_length < 0 ? 411 : 400, "body: one command line of 1 to %d characters",
                CONSOLE_WIFI_CMD_MAX - 1);
        return;
    }
    // Longer than a UART line (#110: a whole contact link), so on the heap rather than this task's stack
    char *line = heap_caps_malloc(CONSOLE_WIFI_CMD_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *out  = heap_caps_malloc(CONSOLE_REPLY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line || !out) {
        free(line);
        free(out);
        respond(fd, 503, "out of memory, try again");
        return;
    }
    if (!read_console_line(fd, req, pre, pre_len, line)) {
        secure_wipe(line, CONSOLE_WIFI_CMD_MAX);
        free(line);
        free(out);
        respond(fd, 400, "couldn't read a printable command line");
        return;
    }
    console_reply_ctx_t ctx = {.fd = fd};
    hooks.console_run(line, out, CONSOLE_REPLY, console_done, &ctx); // may restart the badge, after the reply
    secure_wipe(line, CONSOLE_WIFI_CMD_MAX);
    secure_wipe(out, CONSOLE_REPLY); // the reply may hold a secret line (a channel key, the token; #123)
    free(line);
    free(out);
}

// A screenshot runs on its own short-lived task so a slow link doesn't hold the one server task: the `fw` console and
// uploads keep being served meanwhile (#146). Only one at a time: the screen buffer is a single one.
static volatile bool shot_busy;
static bool          fd_detached; // set by the handler that took the connection over: serve() must not close it

static void close_lingering(int fd) {
    // Let the client close first so the TIME_WAIT lands on its side, not on our port
    set_read_timeout(fd, 0, 500000);
    uint8_t scratch[64];
    for (int i = 0; i < 16 && read(fd, scratch, sizeof(scratch)) > 0; i++) {
    }
    close(fd);
}

static void screenshot_task(void *arg) {
    shot_t *shot = arg;
    int     fd   = shot->fd;
    send_shot(fd, shot->pixels, shot->total, shot->head);
    if (hooks.screenshot_free) {
        hooks.screenshot_free();
    }
    close_lingering(fd);
    free(shot);
    shot_busy = false;
    ESP_LOGI(TAG, "screenshot sent, task stack never below %u bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static void start_screenshot(int fd) {
    if (__atomic_exchange_n(&shot_busy, true, __ATOMIC_ACQ_REL)) {
        respond(fd, 503, "another screenshot is being sent, try again");
        return;
    }
    shot_t *shot = heap_caps_malloc(sizeof(*shot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shot) {
        shot_busy = false;
        respond(fd, 503, "out of memory, try again");
        return;
    }
    if (!shot_render(fd, shot)) {
        free(shot);
        shot_busy = false;
        return;
    }
    if (xTaskCreate(screenshot_task, "ota_shot", SHOT_STACK, shot, 4, NULL) != pdPASS) {
        if (hooks.screenshot_free) {
            hooks.screenshot_free();
        }
        free(shot);
        shot_busy = false;
        respond(fd, 503, "out of memory, try again");
        return;
    }
    fd_detached = true;
}

// Before anything stops (#302): stopping the apps ends in a restart, and a restart on trial rolls the image back
static bool refused_on_trial(int fd, ota_request_t const *req, size_t pre_len) {
    if (!ota_update_refused_on_trial(ota_route_stops_apps(req->route), ota_image_trial_state())) {
        return false;
    }
    drain_body(fd, pre_len, req->expect_continue ? 0 : req->content_length);
    respond(fd, 503, "%s", TRIAL_REFUSAL);
    return true;
}

// "deflate chunks" tells tools/badge.sh this receiver takes compressed bodies and pieces, "zpieces" compressed pieces
// (#298); "storage free N" (#299) lets
// it check a file push fits before it sends a byte
static void respond_info(int fd) {
    uint64_t free_b, total_b;
    char     room[40] = "";
    if (storage_free_bytes(&free_b, &total_b)) {
        snprintf(room, sizeof(room), "; storage free %llu", (unsigned long long)free_b);
    }
    // The hw object makes the line longer than respond's stack buffer: built in the PSRAM body buffer, which no other
    // request uses while this one is answered (#321)
    char *line = (char *)body_buf;
    int   n    = snprintf(line, 256, "badge_fw %s; receiver: deflate chunks zpieces%s%s; hw ", esp_app_get_description()->version,
                          hooks.prepare ? " prepare" : "", room);
    n          = n < 0 ? 0 : n > 255 ? 255 : n; // snprintf says what it would have written
    n         += (int)ota_hw_json(hooks.hw_text, line + n, 1024);
    strcpy(line + n, "\n");
    send_reply(fd, 200, line);
}

// A new firmware boots after the reboot; without one, a reboot on trial goes back to the previous version (#304)
static void respond_reboot(int fd) {
    if (!firmware_installed && system_restart_on_trial()) {
        respond(fd, 200, "rebooting: %s", SYSTEM_RESTART_TRIAL_NOTE);
    } else {
        respond(fd, 200, "rebooting");
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

    if (ota_route_needs_token(req.route) && (!req.has_token || !ota_token_equal(req.token, token))) {
        respond(fd, 401, "wrong or missing token");
        return false;
    }

    if (req.route == OTA_ROUTE_INFO) {
        respond_info(fd);
        return false;
    }

    uint8_t const *pre     = (uint8_t const *)head_buf + head_len;
    size_t         pre_len = len - head_len;
    if (refused_on_trial(fd, &req, pre_len)) {
        return false;
    }
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
        case OTA_ROUTE_SCREENSHOT: start_screenshot(fd); return false;
        case OTA_ROUTE_LOG: handle_log(fd, &req); return false;
        case OTA_ROUTE_CRASHES: handle_crashes(fd); return false;
        case OTA_ROUTE_CONSOLE:
            handle_console(fd, &req, pre, pre_len);
            secure_wipe(head_buf + head_len, pre_len); // the body's first bytes were read into the request buffer
            return false;
        case OTA_ROUTE_REBOOT:
            respond_reboot(fd);
            hook_end(true, firmware_installed); // a plain reboot with no new firmware is not an update
            return true;
        default: return false;
    }
}

#define TOKEN_NVS_KEY "ota_token" // the copy that survives an unmounted /storage (#112)

static bool token_text_valid(char const *text) {
    for (int i = 0; i < OTA_TOKEN_LEN; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool read_token_file(char *out) {
    FILE *f = fopen(TOKEN_FILE, "r");
    if (!f) {
        return false;
    }
    char text[OTA_TOKEN_LEN + 8] = {0};
    fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[OTA_TOKEN_LEN] = '\0';
    if (!token_text_valid(text)) {
        return false;
    }
    strcpy(out, text);
    return true;
}

static bool read_token_nvs(char *out) {
    char text[OTA_TOKEN_LEN + 8] = {0};
    if (!ota_os_secret_get(TOKEN_NVS_KEY, text, sizeof(text))) {
        return false;
    }
    if (strlen(text) != OTA_TOKEN_LEN || !token_text_valid(text)) {
        return false;
    }
    strcpy(out, text);
    return true;
}

static bool write_token_file(void) {
    FILE *f = fopen(TOKEN_FILE, "w");
    if (!f) {
        return false;
    }
    bool written = fprintf(f, "%s\n", token) > 0;
    return fclose(f) == 0 && written;
}

// /storage first (what tools read), NVS as the copy that still works when /storage is not mounted. True when the
// token will be the same next boot.
static bool load_or_create_token(void) {
    bool from_file = read_token_file(token);
    bool from_nvs  = !from_file && read_token_nvs(token);
    if (!from_file && !from_nvs) {
        // The token guards firmware installs: it must not be predictable (lichen_random keeps the entropy source on)
        uint8_t raw[OTA_TOKEN_LEN / 2];
        lichen_random_fill(raw, sizeof(raw));
        ota_hex(raw, sizeof(raw), token);
    }
    bool wrote_file = !from_file && write_token_file();
    bool wrote_nvs  = false;
    char stored[OTA_TOKEN_LEN + 8];
    if (!(from_nvs || (read_token_nvs(stored) && strcmp(stored, token) == 0))) {
        wrote_nvs = ota_os_secret_set(TOKEN_NVS_KEY, token);
    }
    return ota_token_kept(from_file, from_nvs, wrote_file, wrote_nvs);
}

// A listening socket on OTA_PORT whose accept() gives up after a second, so a switch-off is noticed; -1 on failure
static int open_listener(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) {
        ESP_LOGE(TAG, "can't create a socket");
        return -1;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(OTA_PORT), .sin_addr.s_addr = 0};
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 2) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d", OTA_PORT);
        close(ls);
        return -1;
    }
    set_read_timeout(ls, 1, 0);
    return ls;
}

static void serve(int ls) {
    // Closes with Wi-Fi (the OS switch or airplane mode, #108): the C6 stops answering and the IP is gone
    while (server_on && ota_os_wifi_on()) {
        autooff_check();
        if (!server_on) {
            break;
        }
        struct sockaddr_in peer;
        socklen_t          peer_len = sizeof(peer);
        int                fd       = accept(ls, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            piece_expire(false);
            continue;
        }
        piece_expire(false); // also between connections: a /log poll every second must not keep it alive
        set_read_timeout(fd, READ_TIMEOUT_S, 0);
        fd_detached = false;
        bool reboot = handle_connection(fd);
        if (!fd_detached) {
            close_lingering(fd);
        }
        if (reboot) {
            ESP_LOGI(TAG, "reboot requested over Wi-Fi");
            vTaskDelay(pdMS_TO_TICKS(300));
            system_restart_asked("ota reboot over Wi-Fi");
        }
    }
}

static void server_task(void *arg) {
    (void)arg;
    for (;;) {
        // Wi-Fi may come up late or not at all (no credentials), and the server may be switched off (#44)
        // The .local name follows Wi-Fi (#156), the _http service the server switch
        for (;;) {
            bool net = ota_os_wifi_on() && wifi_has_ip(NULL, 0);
            lichen_mdns_update(net, net && server_on, OTA_PORT);
            if (net && server_on) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            autooff_check(); // the time runs from boot, with or without Wi-Fi
        }
        int ls = open_listener();
        if (ls < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        ESP_LOGI(TAG, "update server on port %d", OTA_PORT);
        serve(ls);
        close(ls);
        lichen_mdns_update(ota_os_wifi_on() && wifi_has_ip(NULL, 0), false, OTA_PORT);
        piece_expire(true); // switched off half-way through an upload
        ESP_LOGI(TAG, "update server closed");
    }
}

static void cmd_ota(char const *args) {
    uint8_t minutes;
    if (strcmp(args, "on") == 0 || strcmp(args, "off") == 0) {
        ota_server_set_enabled(args[1] == 'n');
    } else if (strncmp(args, "auto ", 5) == 0 && ota_autooff_parse(args + 5, &minutes)) {
        ota_server_set_auto_off(minutes);
    } else if (*args) {
        console_reply("usage: ota [on|off | auto <off|1-%d minutes>]", OTA_AUTOOFF_MAX_MIN);
        return;
    }
    if (token_refused) {
        console_reply("update server over Wi-Fi: refused this boot (no saved token); USB flashing still works");
        return;
    }
    char     when[16];
    uint8_t  min  = auto_min;
    uint32_t left = ota_autooff_left(min, armed_s, (uint32_t)now_s());
    ota_autooff_text(min, when, sizeof(when));
    if (server_on && min) {
        console_reply("update server over Wi-Fi: on, closes after %s (%lu s left)", when, (unsigned long)left);
    } else if (auto_closed) {
        console_reply("update server over Wi-Fi: closed by its %s auto-off for this boot (setting on)", when);
    } else {
        console_reply("update server over Wi-Fi: %s, auto-off %s", server_on ? "on" : "off", min ? when : "never");
    }
}

static void cmd_token(char const *args) {
    (void)args;
    console_reply_secret("token %s", token[0] ? token : "(none: no /storage and no settings; Wi-Fi updates refused)");
}

void ota_server_start(void) {
    console_register("ota", "ota [on|off | auto <off|minutes>] (the update server over Wi-Fi, also Settings)",
                     cmd_ota);
    ota_trial_register_cmds();
    server_on = ota_os_get_u8(OTA_WIFI_KEY, OTA_WIFI_DEFAULT) != 0;
    uint8_t m = ota_os_get_u8(OTA_AUTO_KEY, 0);
    auto_min  = m <= OTA_AUTOOFF_MAX_MIN ? m : 0;
    armed_s   = (uint32_t)now_s(); // boot
    token_refused = !load_or_create_token();
    if (token_refused) {
        // An empty token also fails every comparison (ota_token_equal), should anything still reach the check
        secure_wipe(token, sizeof(token));
        ESP_LOGE(TAG, "Wi-Fi updates refused this boot: no saved token (no /storage, no settings); USB flashing works");
    }
    // After the token is final (#5): "fw token" never shows a half-written one or "(none)" while it is being made
    console_register("token", "token (the update token for tools/badge.sh)", cmd_token);
    if (token_refused) {
        return; // no listener, no mDNS service, no Wi-Fi console
    }
    if (xTaskCreatePinnedToCore(server_task, "ota", SERVER_STACK, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "update server task failed to start");
    }
}
