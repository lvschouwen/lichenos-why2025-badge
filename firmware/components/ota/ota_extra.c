// The update server's other endpoints (#447, from ota_server.c): GET /info, /screenshot (sent from its own task),
// /log and /crashes, and POST /console. Their bodies are private or secret: never logged here.
#include "ota_server_priv.h"

#include "console_cmd.h" // CONSOLE_WIFI_CMD_MAX
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_bmp.h"
#include "ota_hold.h"
#include "ota_hw.h"
#include "ota_rle.h"
#include "secure_wipe.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SHOT_SLICE        (16 * 1024) // bytes of pixels compressed and sent per round of a screenshot
#define SHOT_SEND_S       4   // one send of a screenshot (about 10x fewer bytes than raw, so a dead link frees the task fast)
#define SHOT_TOTAL_S      10  // whole screenshot send (#146)
#define BODY_SEND_S       10
#define LOG_SLICE         2048  // bytes copied out of the log store per call (interrupts are off meanwhile)
#define CRASHES_REPLY_MAX (12 * 1024) // 20 records of 256 bytes and the header
#define LOG_REPLY_MAX     (24 * 1024) // one GET /log answer; the client asks again from X-Next-Seq
#define CONSOLE_REPLY     4096
#define BODY_TOTAL_S      45 // whole send: a client that drips bytes must not hold the one server task
// The screenshot worker only compresses and sends from PSRAM, so a small internal stack does (#146)
#define SHOT_STACK        4096

static char const TAG[] = "ota";

// The body: the BMP header, then the pixels as ota_rle packets, compressed slice by slice into one small PSRAM buffer
// (a UI frame is mostly flat colour: about 10:1). There is no Content-Length (the size isn't known up front), so the
// client checks the pixel count; a cut connection leaves it short.
static void send_shot(int fd, uint8_t const *pixels, size_t total, uint8_t const *head) {
    uint8_t *out = heap_caps_malloc(ota_rle_bound(SHOT_SLICE / 2), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) {
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    struct timeval tv = {.tv_sec = SHOT_SEND_S};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    static char const reply[] = "HTTP/1.1 200 OK\r\nContent-Type: application/x-lichen-bmp-rle\r\n"
                                "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    bool ok   = ota_send_all(fd, reply, sizeof(reply) - 1) && ota_send_all(fd, head, OTA_BMP_HEADER_LEN);
    long stop = ota_now_s() + SHOT_TOTAL_S;
    for (size_t off = 0; ok && off < total; off += SHOT_SLICE) {
        size_t n = total - off < SHOT_SLICE ? total - off : SHOT_SLICE;
        ok       = ota_now_s() <= stop && ota_send_all(fd, out, ota_rle_encode(pixels + off, n / 2, out));
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
    if (!ota_hooks.screenshot) {
        ota_respond(fd, 404, "no screen here");
        return false;
    }
    if (!file_allowed()) {
        ota_respond(fd, 409, "a firmware update is running");
        return false;
    }
    uint8_t const *pixels;
    uint32_t       w, h, stride;
    int            status = ota_hooks.screenshot(&pixels, &w, &h, &stride);
    if (status) {
        ota_respond(fd, status, status == 503 ? "the screen is busy, try again" : "no screenshot");
        return false;
    }
    size_t row  = (size_t)w * 2;
    size_t size = ota_bmp565_file_size(w, h);
    // Rows go out as they sit in memory: only a tightly packed picture with 4-byte rows is supported
    if (!size || stride != row || ota_bmp565_row_bytes(w) != row || !ota_bmp565_header(w, h, shot->head)) {
        ota_respond(fd, 500, "unsupported screen shape");
        if (ota_hooks.screenshot_free) {
            ota_hooks.screenshot_free();
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
    bool ok   = ota_send_all(fd, head, head_len);
    long stop = ota_now_s() + BODY_TOTAL_S;
    for (size_t off = 0; ok && off < len; off += SHOT_SLICE) {
        ok = ota_now_s() <= stop && ota_send_all(fd, body + off, len - off < SHOT_SLICE ? len - off : SHOT_SLICE);
    }
}

// GET /log (#38): lines of the log store from ?since= on. At most LOG_REPLY_MAX bytes are collected first (copied out of
// the store in small locked slices), so the headers can carry the exact length and the next sequence number. Lines
// are private (node names, messages): never logged by this server, token required like the rest.
static void handle_log(int fd, ota_request_t const *req) {
    if (!ota_hooks.log_read) {
        ota_respond(fd, 404, "no log here");
        return;
    }
    char *reply = heap_caps_malloc(LOG_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    uint32_t since  = req->since > UINT32_MAX ? UINT32_MAX : (uint32_t)req->since;
    uint32_t next   = since;
    uint32_t missed = 0;
    size_t   total  = 0;
    while (total + LOG_SLICE <= LOG_REPLY_MAX) {
        uint32_t step_missed;
        size_t   n = ota_hooks.log_read(since, (char *)ota_body_buf, LOG_SLICE, &next, &step_missed);
        missed    += step_missed;
        if (n == 0) {
            break;
        }
        memcpy(reply + total, ota_body_buf, n);
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
    if (!ota_hooks.crashes_read) {
        ota_respond(fd, 404, "no crash export here");
        return;
    }
    char *reply = heap_caps_malloc(CRASHES_REPLY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!reply) {
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    size_t total = ota_hooks.crashes_read(reply, CRASHES_REPLY_MAX);
    if (total == 0 || total > CRASHES_REPLY_MAX) {
        ota_respond(fd, 503, "the crash log could not be read, try again");
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
    long deadline = ota_now_s() + HEAD_DEADLINE_S;
    ota_set_read_timeout(fd, READ_TIMEOUT_S, 0);
    while (have < want) {
        ssize_t n = read(fd, line + have, want - have);
        if (n <= 0 || ota_now_s() > deadline) {
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
    if (!ota_hooks.console_run) {
        ota_respond(fd, 404, "the Wi-Fi console is off");
        return;
    }
    if (!file_allowed()) {
        ota_respond(fd, 409, "a firmware update is running");
        return;
    }
    if (!console_wifi_len_ok(req->content_length)) {
        ota_respond(fd, req->content_length < 0 ? 411 : 400, "body: one command line of 1 to %d characters",
                    CONSOLE_WIFI_CMD_MAX - 1);
        return;
    }
    // Longer than a UART line (#110: a whole contact link), so on the heap rather than this task's stack
    char *line = heap_caps_malloc(CONSOLE_WIFI_CMD_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *out  = heap_caps_malloc(CONSOLE_REPLY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line || !out) {
        free(line);
        free(out);
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    if (!read_console_line(fd, req, pre, pre_len, line)) {
        secure_wipe(line, CONSOLE_WIFI_CMD_MAX);
        free(line);
        free(out);
        ota_respond(fd, 400, "couldn't read a printable command line");
        return;
    }
    console_reply_ctx_t ctx = {.fd = fd};
    ota_hooks.console_run(line, out, CONSOLE_REPLY, console_done, &ctx); // may restart the badge, after the reply
    secure_wipe(line, CONSOLE_WIFI_CMD_MAX);
    secure_wipe(out, CONSOLE_REPLY); // the reply may hold a secret line (a channel key, the token; #123)
    free(line);
    free(out);
}

// A screenshot runs on its own short-lived task so a slow link doesn't hold the one server task: the `fw` console and
// uploads keep being served meanwhile (#146). Only one at a time: the screen buffer is a single one.
static volatile bool shot_busy;

static void screenshot_task(void *arg) {
    shot_t *shot = arg;
    int     fd   = shot->fd;
    send_shot(fd, shot->pixels, shot->total, shot->head);
    if (ota_hooks.screenshot_free) {
        ota_hooks.screenshot_free();
    }
    ota_close_lingering(fd);
    free(shot);
    shot_busy = false;
    ESP_LOGI(TAG, "screenshot sent, task stack never below %u bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

static void start_screenshot(int fd) {
    if (__atomic_exchange_n(&shot_busy, true, __ATOMIC_ACQ_REL)) {
        ota_respond(fd, 503, "another screenshot is being sent, try again");
        return;
    }
    shot_t *shot = heap_caps_malloc(sizeof(*shot), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!shot) {
        shot_busy = false;
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    if (!shot_render(fd, shot)) {
        free(shot);
        shot_busy = false;
        return;
    }
    if (xTaskCreate(screenshot_task, "ota_shot", SHOT_STACK, shot, 4, NULL) != pdPASS) {
        if (ota_hooks.screenshot_free) {
            ota_hooks.screenshot_free();
        }
        free(shot);
        shot_busy = false;
        ota_respond(fd, 503, "out of memory, try again");
        return;
    }
    ota_fd_detached = true;
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
    // The hw object makes the line longer than ota_respond's stack buffer: built in the PSRAM body buffer, which no other
    // request uses while this one is answered (#321)
    char *line = (char *)ota_body_buf;
    int   n    = snprintf(line, 256, "badge_fw %s; receiver: deflate chunks zpieces%s%s; hw ", esp_app_get_description()->version,
                          ota_hooks.prepare ? " prepare" : "", room);
    n          = n < 0 ? 0 : n > 255 ? 255 : n; // snprintf says what it would have written
    n         += (int)ota_hw_json(ota_hooks.hw_text, line + n, 1024);
    strcpy(line + n, "\n");
    ota_send_reply(fd, 200, line);
}

bool ota_on_info(ota_conn_t const *conn) {
    respond_info(conn->fd);
    return false;
}

bool ota_on_screenshot(ota_conn_t const *conn) {
    start_screenshot(conn->fd);
    return false;
}

bool ota_on_log(ota_conn_t const *conn) {
    handle_log(conn->fd, conn->req);
    return false;
}

bool ota_on_crashes(ota_conn_t const *conn) {
    handle_crashes(conn->fd);
    return false;
}

bool ota_on_console(ota_conn_t const *conn) {
    handle_console(conn->fd, conn->req, conn->pre, conn->pre_len);
    secure_wipe((void *)conn->pre, conn->pre_len); // the body's first bytes were read into the request buffer
    return false;
}
