// The MeshCore app's Radio health page (#157): MeshCore's own counters (what upstream's CMD_GET_STATS reports) and the
// radio's channel reading as read-only rows, refreshed once a second from one snapshot taken under the mesh lock
// (mesh_health_snapshot, 20 ms at most: a busy MeshCore keeps the last values). Receive only, nothing is sent.
#include "screen_ids.h"
#include "lichen_ui.h"
#include "menu.h"
#include "mesh.h"
#include "meshcore_screens.h"

#include <stdio.h>
#include <string.h>

#define REFRESH_MS     1000
#define SCREEN_WAIT_MS 20
#define VALUE_MAX      48

enum { ROW_NOISE, ROW_RSSI, ROW_SNR, ROW_TX_AIR, ROW_RX_AIR, ROW_RECV, ROW_SENT, ROW_DROPS, ROW_EVENTS, ROW_QUEUE,
       ROW_UPTIME, ROWS };

static char const *const LABELS[ROWS] = {
    [ROW_NOISE] = "Noise floor", [ROW_RSSI] = "Last signal", [ROW_SNR] = "Last SNR", [ROW_TX_AIR] = "Transmit time",
    [ROW_RX_AIR] = "Receive time", [ROW_RECV] = "Received", [ROW_SENT] = "Sent", [ROW_DROPS] = "Lost packets",
    [ROW_EVENTS] = "Radio events", [ROW_QUEUE] = "Send queue", [ROW_UPTIME] = "MeshCore uptime",
};

static struct {
    lv_obj_t   *row[ROWS];
    char        shown[ROWS][VALUE_MAX];
    lv_timer_t *timer;
    lv_obj_t   *screen;
} g;

// Only on a change: the same text every second would redraw for nothing
static void show(int i, char const *text) {
    if (g.row[i] && strcmp(g.shown[i], text) != 0) {
        strlcpy(g.shown[i], text, VALUE_MAX);
        menu_set_value(g.row[i], text);
    }
}

static void show_missing(void) {
    for (int i = 0; i < ROWS; i++) {
        show(i, "-");
    }
}

static void show_health(mesh_health_t const *h) {
    char t[VALUE_MAX];
    if (h->have_noise) {
        mh_fmt_dbm(h->noise_x10, t, sizeof(t));
    } else {
        strlcpy(t, "-", sizeof(t));
    }
    show(ROW_NOISE, t);
    if (h->have_last) {
        mh_fmt_dbm(h->last_rssi_x10, t, sizeof(t));
        show(ROW_RSSI, t);
        mh_fmt_db(h->last_snr_x10, t, sizeof(t));
        show(ROW_SNR, t);
    } else {
        show(ROW_RSSI, "none heard yet");
        show(ROW_SNR, "none heard yet");
    }
    mh_fmt_air(h->tx_air_ms, h->uptime_s, t, sizeof(t));
    show(ROW_TX_AIR, t);
    mh_fmt_air(h->rx_air_ms, h->uptime_s, t, sizeof(t));
    show(ROW_RX_AIR, t);
    mh_fmt_split(h->recv_flood, h->recv_direct, t, sizeof(t));
    show(ROW_RECV, t);
    mh_fmt_split(h->sent_flood, h->sent_direct, t, sizeof(t));
    show(ROW_SENT, t);
    mh_fmt_drops(h, t, sizeof(t));
    show(ROW_DROPS, t);
    mh_fmt_errors(h->err_flags, t, sizeof(t));
    show(ROW_EVENTS, t);
    snprintf(t, sizeof(t), "%u waiting", (unsigned)h->queue_len);
    show(ROW_QUEUE, t);
    mh_fmt_uptime(h->uptime_s, t, sizeof(t));
    show(ROW_UPTIME, t);
}

static void refresh(lv_timer_t *timer) {
    if (timer && lv_screen_active() != g.screen) {
        return; // something is on top: no mesh lock for rows nobody sees
    }
    mesh_health_t h;
    if (mesh_health_snapshot(&h, SCREEN_WAIT_MS)) {
        show_health(&h);
    } else if (!mesh_running()) {
        show_missing(); // MeshCore stopped: nothing to read; a busy lock keeps the last values
    }
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (g.timer) {
        lv_timer_delete(g.timer);
    }
    memset(&g, 0, sizeof(g));
}

static lv_obj_t *build(void) {
    lv_obj_t *screen = shell_page_create();
    memset(&g, 0, sizeof(g));
    g.screen         = screen;
    lv_obj_t *menu   = menu_create(screen, "Radio health");
    // Read-only rows: with no selectable row the menu takes the focus, opens at the top and Up/Down scroll it (#174)
    for (int i = 0; i < ROWS; i++) {
        g.row[i] = menu_add(menu, LABELS[i], "", NULL, NULL);
        menu_set_kind(g.row[i], MENU_INFO);
    }
    lichen_ui_event_add(screen, on_delete, LV_EVENT_DELETE, NULL);
    g.timer = lichen_ui_timer_create(refresh, REFRESH_MS, NULL);
    refresh(NULL); // not loaded yet: the values right away all the same
    return screen;
}

void meshcore_health_open(void *arg) {
    (void)arg;
    shell_open(build, SCREEN_MC_HEALTH);
}
