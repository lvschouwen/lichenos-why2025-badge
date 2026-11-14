// The update server's HTTP side (#447, from ota_server.c): replies, request bodies (received, decoded, hashed and
// handed to a sink) and the close. Every reply status, header and text is the one the server always sent.
#include "ota_server_priv.h"

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h" // the ROM inflater
#include "ota_inflate.h"
#include "sha256.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define BODY_MIN_RATE (16 * 1024) // bytes/s a body must average, on top of BODY_GRACE_S
#define BODY_GRACE_S  30

// One request at a time
EXT_RAM_BSS_ATTR uint8_t ota_body_buf[OTA_BODY_BUF_LEN];
uint32_t                 ota_progress_base;
long long ota_last_received;
static long long last_decoded;  // file bytes they decoded to

long ota_now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

bool ota_send_all(int fd, void const *data, size_t len) {
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

// The reply head and a body (NUL terminated, ends in a newline), sent as is
void ota_send_reply(int fd, int status, char const *body) {
    char head[192];
    int  n = snprintf(
        head,
        sizeof(head),
        "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
        status,
        ota_status_reason(status),
        (unsigned)strlen(body)
    );
    if (ota_send_all(fd, head, (size_t)n)) {
        ota_send_all(fd, body, strlen(body));
    }
}

void ota_respond(int fd, int status, char const *fmt, ...) {
    char    body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body) - 1, fmt, ap);
    va_end(ap);
    strcat(body, "\n");
    ota_send_reply(fd, status, body);
}

void ota_set_read_timeout(int fd, int seconds, int usec) {
    struct timeval tv = {.tv_sec = seconds, .tv_usec = usec};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

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

// One read of the body: what sits behind the head first, then the socket within the body's deadline
static ssize_t body_chunk(int fd, body_t *b, long long got, long deadline, uint8_t const **chunk) {
    if (b->pre_len > 0) {
        size_t n   = b->pre_len;
        *chunk     = b->pre;
        b->pre_len = 0;
        return (ssize_t)n;
    }
    if (ota_now_s() > deadline) {
        return -1;
    }
    size_t want = b->total - got < (long long)sizeof(ota_body_buf) ? (size_t)(b->total - got) : sizeof(ota_body_buf);
    *chunk      = ota_body_buf;
    return read(fd, ota_body_buf, want);
}

// Read the body, decode it if it is compressed, hash it and feed it to its sink. sha_hex is the SHA-256 of the decoded
// bytes, i.e. of the file as it ends up. A compressed body must end its stream.
bool ota_receive_body(int fd, body_t b, char sha_hex[OTA_SHA256_HEX + 1]) {
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
    long      deadline = ota_now_s() + BODY_GRACE_S + (long)(b.total / BODY_MIN_RATE);
    long long got      = 0;
    ota_last_received      = 0;
    last_decoded       = 0;
    hook_progress(ota_progress_base);
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
        ota_last_received  = got;
        hook_progress(ota_progress_base + (b.piece ? last_decoded : got));
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

// Read and drop a refused body (at most REFUSE_DRAIN_MAX bytes, within REFUSE_DRAIN_S) so the reply isn't lost to the
// reset a close with unread data causes; a larger body is left: badge.sh checks the space before it sends (#299)
#define REFUSE_DRAIN_MAX (256 * 1024)
#define REFUSE_DRAIN_S   5
void ota_drain_body(int fd, size_t pre_len, long long content_length) {
    if (content_length <= 0 || content_length > REFUSE_DRAIN_MAX || (long long)pre_len >= content_length) {
        return;
    }
    long long left = content_length - (long long)pre_len;
    long      stop = ota_now_s() + REFUSE_DRAIN_S;
    uint8_t   scratch[256];
    ota_set_read_timeout(fd, 1, 0);
    while (left > 0 && ota_now_s() < stop) {
        ssize_t n = read(fd, scratch, left < (long long)sizeof(scratch) ? (size_t)left : sizeof(scratch));
        if (n <= 0) {
            break;
        }
        left -= n;
    }
}

void ota_close_lingering(int fd) {
    // Let the client close first so the TIME_WAIT lands on its side, not on our port
    ota_set_read_timeout(fd, 0, 500000);
    uint8_t scratch[64];
    for (int i = 0; i < 16 && read(fd, scratch, sizeof(scratch)) > 0; i++) {
    }
    close(fd);
}
