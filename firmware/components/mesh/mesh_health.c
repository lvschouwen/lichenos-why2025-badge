#include "mesh_health.h"

#include <stdio.h>
#include <string.h>

static int round_x10(int16_t x10) {
    return x10 >= 0 ? (x10 + 5) / 10 : -((-x10 + 5) / 10);
}

void mh_fmt_dbm(int16_t x10, char *out, size_t cap) {
    snprintf(out, cap, "%d dBm", round_x10(x10));
}

void mh_fmt_db(int16_t x10, char *out, size_t cap) {
    int a = x10 < 0 ? -x10 : x10;
    snprintf(out, cap, "%s%d.%d dB", x10 < 0 ? "-" : "", a / 10, a % 10);
}

// The share of the uptime in whole and tenth percent; "<0.1%" when there is some airtime but less than that
static void share(char *out, size_t cap, uint32_t air_ms, uint32_t uptime_s) {
    if (air_ms == 0 || uptime_s == 0) {
        snprintf(out, cap, "0%%");
        return;
    }
    // tenths of a percent: air_ms / (uptime_s * 1000) * 1000
    uint64_t tenths = (uint64_t)air_ms / (uint64_t)uptime_s;
    if (tenths > 9999) {
        tenths = 9999; // cannot be that much of the uptime; keeps the text short
    }
    if (tenths == 0) {
        snprintf(out, cap, "<0.1%%");
    } else {
        snprintf(out, cap, "%u.%u%%", (unsigned)(tenths / 10), (unsigned)(tenths % 10));
    }
}

void mh_fmt_air(uint32_t air_ms, uint32_t uptime_s, char *out, size_t cap) {
    char pct[16];
    share(pct, sizeof(pct), air_ms, uptime_s);
    if (air_ms < 1000) {
        snprintf(out, cap, "%u ms (%s)", (unsigned)air_ms, pct);
    } else if (air_ms < 10000) {
        snprintf(out, cap, "%u.%u s (%s)", (unsigned)(air_ms / 1000), (unsigned)(air_ms % 1000 / 100), pct);
    } else if (air_ms < 120000) {
        snprintf(out, cap, "%u s (%s)", (unsigned)(air_ms / 1000), pct);
    } else {
        snprintf(out, cap, "%u min (%s)", (unsigned)(air_ms / 60000), pct);
    }
}

void mh_fmt_uptime(uint32_t s, char *out, size_t cap) {
    if (s < 60) {
        snprintf(out, cap, "%u s", (unsigned)s);
    } else if (s < 3600) {
        snprintf(out, cap, "%u min", (unsigned)(s / 60));
    } else if (s < 86400) {
        snprintf(out, cap, "%u h %u min", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
    } else {
        snprintf(out, cap, "%u d %u h", (unsigned)(s / 86400), (unsigned)(s % 86400 / 3600));
    }
}

static void add(char *out, size_t cap, char const *text) {
    size_t n = strlen(out);
    snprintf(out + n, cap - n, "%s%s", n ? ", " : "", text);
}

void mh_fmt_errors(uint16_t flags, char *out, size_t cap) {
    out[0] = '\0';
    if (flags & MH_ERR_FULL) {
        add(out, cap, "queue full");
    }
    if (flags & MH_ERR_CAD) {
        add(out, cap, "channel busy");
    }
    if (flags & MH_ERR_STARTRX) {
        add(out, cap, "rx timeout");
    }
    if (!out[0]) {
        snprintf(out, cap, "none");
    }
}

void mh_fmt_split(uint32_t flood, uint32_t direct, char *out, size_t cap) {
    snprintf(out, cap, "%u flood, %u direct", (unsigned)flood, (unsigned)direct);
}

void mh_fmt_drops(mesh_health_t const *h, char *out, size_t cap) {
    out[0] = '\0';
    char part[32];
    if (h->rx_dropped) {
        snprintf(part, sizeof(part), "%u dropped", (unsigned)h->rx_dropped);
        add(out, cap, part);
    }
    if (h->send_failed) {
        snprintf(part, sizeof(part), "%u failed send%s", (unsigned)h->send_failed, h->send_failed == 1 ? "" : "s");
        add(out, cap, part);
    }
    if (h->recv_errors) {
        snprintf(part, sizeof(part), "%u bad", (unsigned)h->recv_errors);
        add(out, cap, part);
    }
    if (!out[0]) {
        snprintf(out, cap, "none");
    }
}
