// Wi-Fi update server of the lean firmware (#61), adapted from BadgeVMS's OTA receiver app
// (sdk_apps/ota_receiver/main.c): same protocol (ota_proto.h) so tools/badge.sh ota-* works unchanged. It runs
// whenever Wi-Fi is up and Settings › Updates over Wi-Fi is on (#44); the token guards every route. Bodies are
// received into memory and checked before anything touches flash: flash writes while the network is receiving corrupted the stream on this badge (#40).
// This file keeps the listener, the settings, the hooks and the dispatch; the routes live in ota_upload.c,
// ota_piece_http.c and ota_extra.c, the HTTP side in ota_http.c, the token in ota_token.c (#447).
#include "ota_server_priv.h"
#include "ota_token.h"

#include "esp_attr.h"

#include "c6_paths.h"
#include "lichen_mdns.h"
#include "wifi_service.h"
#include "fw_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_autooff.h"
#include "ota_path.h"
#include "ota_hold.h"
#include "ota_trial.h"
#include "ota_valid.h"
#include "ota_os.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// Flash writes happen on this task, so its stack must be in internal RAM; peaks measured in BadgeVMS: 7 KB
#define SERVER_STACK    16384

static char const TAG[] = "ota";

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

bool ota_server_enabled(void) {
    return ota_server_listens(server_on, !token_refused);
}

bool ota_server_refused(void) {
    return token_refused;
}

void ota_server_set_enabled(bool on) {
    if (on) {
        armed_s = (uint32_t)ota_now_s(); // the auto-off counts from now
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
    armed_s  = (uint32_t)ota_now_s(); // a new time counts from now, not from boot
    auto_min = minutes;
    ota_os_set_u8(OTA_AUTO_KEY, minutes);
    ESP_LOGI(TAG, "update server auto-off: %u min (0 = never)", (unsigned)minutes);
}

bool ota_server_auto_closed(void) {
    return auto_closed;
}

// One request at a time; PSRAM (#107): sockets copy, no DMA
EXT_RAM_BSS_ATTR static char head_buf[2048];

ota_hooks_t ota_hooks;
bool        ota_firmware_installed;
bool        ota_fd_detached;

void ota_server_set_hooks(ota_hooks_t const *h) {
    ota_hooks = *h;
}

// Closes the server when its auto-off time has passed (#110); never during an upload in pieces. Runs about once a
// second on this task, between connections (one running upload is never cut).
static void autooff_check(void) {
    if (server_on && ota_autooff_due(auto_min, armed_s, (uint32_t)ota_now_s(), ota_piece_active())) {
        server_on   = false; // this boot only: the saved setting stays on
        auto_closed = true;
        ESP_LOGI(
            TAG,
            "update server closed by its %u min auto-off (Settings or \"fw ota on\" opens it again)",
            (unsigned)auto_min
        );
    }
}

// Before anything stops (#302): stopping the apps ends in a restart, and a restart on trial rolls the image back
static bool refused_on_trial(int fd, ota_request_t const *req, size_t pre_len) {
    if (!ota_update_refused_on_trial(ota_route_stops_apps(req->route), ota_image_trial_state())) {
        return false;
    }
    ota_drain_body(fd, pre_len, req->expect_continue ? 0 : req->content_length);
    ota_respond(fd, 503, "%s", TRIAL_REFUSAL);
    return true;
}

// The console's USB hold (#420), asked only on the held routes. A refused piece also drops its upload in pieces (open
// slot, staging buffer), so update mode ends (a firmware one in a restart) instead of waiting PIECE_IDLE_S. noinline:
// the path buffer stays off handle_connection's stack.
__attribute__((noinline)) static bool refused_on_hold(int fd, ota_request_t const *req, size_t pre_len) {
    char const *why = ota_route_usb_held(req->route, true) ? console_usb_hold_why() : NULL; // the list off: no more
    char        posix[OTA_PATH_MAX + 16];
    bool c6 = why && req->has_path && ota_path_to_posix(req->path, posix, sizeof(posix)) && c6_path_in_image_dir(posix);
    ota_hold_t kind = why ? ota_route_usb_held(req->route, c6) : OTA_HOLD_NONE;
    if (kind == OTA_HOLD_NONE) {
        return false;
    }
    ota_piece_expire(ota_piece_staged_is(req, c6, posix));
    ota_drain_body(fd, pre_len, req->expect_continue ? 0 : req->content_length);
    return ota_hold_refused(fd, kind, why);
}

static bool refused_up_front(int fd, ota_request_t const *req, size_t pre_len) {
    return refused_on_trial(fd, req, pre_len) || refused_on_hold(fd, req, pre_len);
}

// A new firmware boots after the reboot; without one, a reboot on trial goes back to the previous version (#304)
static void respond_reboot(int fd) {
    if (!ota_firmware_installed && system_restart_on_trial()) {
        ota_respond(fd, 200, "rebooting: %s", SYSTEM_RESTART_TRIAL_NOTE);
    } else {
        ota_respond(fd, 200, "rebooting");
    }
}

bool ota_on_prepare(ota_conn_t const *conn) {
    if (!ota_hooks.prepare) {
        ota_respond(conn->fd, 404, "no prepare here");
        return false;
    }
    ota_hooks.prepare();
    vTaskDelay(pdMS_TO_TICKS(300)); // freed stacks go back to the heap from the idle task
    ota_respond(conn->fd, 200, "ready dma_free=%u dma_min=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DMA));
    return false;
}

bool ota_on_reboot(ota_conn_t const *conn) {
    respond_reboot(conn->fd);
    hook_end(true, ota_firmware_installed); // a plain reboot with no new firmware is not an update
    return true;
}

// Read a request head into head_buf (*len bytes read in all, the body's first ones included): its length, or 0 when
// the client went quiet, closed or sent a head too large (answered)
static size_t read_head(int fd, size_t *len) {
    size_t head_len = 0;
    long   deadline = ota_now_s() + HEAD_DEADLINE_S;
    while (!head_len) {
        if (ota_now_s() > deadline) {
            return 0;
        }
        if (*len == sizeof(head_buf)) {
            ota_respond(fd, 400, "request head too large");
            return 0;
        }
        ssize_t n = read(fd, head_buf + *len, sizeof(head_buf) - *len);
        if (n <= 0) {
            return 0;
        }
        *len     += (size_t)n;
        head_len  = ota_head_length(head_buf, *len);
    }
    return head_len;
}

// Handle one connection. Returns true when the badge should reboot. ota_route_open is the only way to a handler: it
// checks the token first, for every route (ota_route.c, host_tests/ota_route_test.c).
static bool handle_connection(int fd) {
    size_t len      = 0;
    size_t head_len = read_head(fd, &len);
    if (!head_len) {
        return false;
    }
    ota_request_t req;
    ota_verdict_t verdict;
    ota_route_fn  handler = ota_route_open(head_buf, head_len, ota_token_get(), &req, &verdict);
    if (verdict.status) {
        ota_respond(fd, verdict.status, "%s", verdict.why);
        return false;
    }
    ota_conn_t const conn = {
        .fd = fd, .req = &req, .pre = (uint8_t const *)head_buf + head_len, .pre_len = len - head_len};
    if (!handler || (verdict.up_front && refused_up_front(fd, &req, conn.pre_len))) {
        return false;
    }
    return handler(&conn);
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
    ota_set_read_timeout(ls, 1, 0);
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
            ota_piece_expire(false);
            continue;
        }
        ota_piece_expire(false); // also between connections: a /log poll every second must not keep it alive
        ota_set_read_timeout(fd, READ_TIMEOUT_S, 0);
        ota_fd_detached = false;
        bool reboot = handle_connection(fd);
        if (!ota_fd_detached) {
            ota_close_lingering(fd);
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
        ota_piece_expire(true); // switched off half-way through an upload
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
    uint32_t left = ota_autooff_left(min, armed_s, (uint32_t)ota_now_s());
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
    char const *token = ota_token_get();
    console_reply_secret("token %s", token[0] ? token : "(none: no /storage and no settings; Wi-Fi updates refused)");
}

void ota_server_start(void) {
    console_register("ota", "ota [on|off | auto <off|minutes>] (the update server over Wi-Fi, also Settings)",
                     cmd_ota);
    ota_trial_register_cmds();
    server_on = ota_os_get_u8(OTA_WIFI_KEY, OTA_WIFI_DEFAULT) != 0;
    uint8_t m = ota_os_get_u8(OTA_AUTO_KEY, 0);
    auto_min  = m <= OTA_AUTOOFF_MAX_MIN ? m : 0;
    armed_s   = (uint32_t)ota_now_s(); // boot
    token_refused = !ota_token_load();
    if (token_refused) {
        // An empty token also fails every comparison (ota_token_equal), should anything still reach the check
        ota_token_forget();
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
