#include "mesh_radar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EARTH_M 6371000.0
#define DEG2RAD (3.14159265358979323846 / 180.0)

double radar_distance_m(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * DEG2RAD, p2 = lat2 * DEG2RAD;
    double dp = p2 - p1, dl = (lon2 - lon1) * DEG2RAD;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    if (!(a >= 0.0)) {
        a = 0.0; // NaN from a bad input must not reach a cast
    } else if (a > 1.0) {
        a = 1.0;
    }
    return 2.0 * EARTH_M * asin(sqrt(a));
}

double radar_bearing_deg(double lat1, double lon1, double lat2, double lon2) {
    double p1 = lat1 * DEG2RAD, p2 = lat2 * DEG2RAD, dl = (lon2 - lon1) * DEG2RAD;
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) / DEG2RAD;
    if (b < 0) {
        b += 360.0;
    }
    return b >= 360.0 ? 0.0 : b;
}

static int cmp_entry(void const *pa, void const *pb) {
    radar_entry_t const *a = pa, *b = pb;
    if (a->dist_m != b->dist_m) {
        return a->dist_m < b->dist_m ? -1 : 1;
    }
    int c = strcmp(a->name, b->name);
    return c ? c : memcmp(a->prefix, b->prefix, sizeof(a->prefix));
}

// The bearing is filled in for the kept entries only (radar_build), the loop needs the distance alone
static void fill_entry(radar_entry_t *e, radar_contact_t const *c, size_t src, double own_lat, double own_lon) {
    memcpy(e->prefix, c->prefix, sizeof(e->prefix));
    memcpy(e->name, c->name, sizeof(e->name));
    e->name[sizeof(e->name) - 1] = 0;
    e->type   = c->type;
    e->src    = (uint16_t)src;
    e->dist_m = (uint32_t)(radar_distance_m(own_lat, own_lon, c->lat_e6 / 1e6, c->lon_e6 / 1e6) + 0.5);
}

// A position another node advertised is not trusted: out of range means garbage, left out
static bool position_ok(radar_contact_t const *c) {
    if (c->lat_e6 == 0 && c->lon_e6 == 0) {
        return false; // shares no position
    }
    return c->lat_e6 >= -90000000 && c->lat_e6 <= 90000000 && c->lon_e6 >= -180000000 && c->lon_e6 <= 180000000;
}

size_t radar_build(double own_lat, double own_lon, radar_contact_t const *in, size_t n, radar_entry_t *out, size_t max) {
    size_t k = 0;
    for (size_t i = 0; i < n && max && i <= UINT16_MAX; i++) {
        if (!position_ok(&in[i])) {
            continue;
        }
        radar_entry_t e;
        fill_entry(&e, &in[i], i, own_lat, own_lon);
        if (k < max) {
            out[k++] = e;
            continue;
        }
        size_t far = 0; // full: the new one replaces the farthest if it is closer
        for (size_t j = 1; j < k; j++) {
            far = cmp_entry(&out[j], &out[far]) > 0 ? j : far;
        }
        if (cmp_entry(&e, &out[far]) < 0) {
            out[far] = e;
        }
    }
    for (size_t j = 0; j < k; j++) {
        radar_contact_t const *c = &in[out[j].src];
        out[j].bearing_deg = (uint16_t)(radar_bearing_deg(own_lat, own_lon, c->lat_e6 / 1e6, c->lon_e6 / 1e6) + 0.5) % 360;
    }
    qsort(out, k, sizeof(*out), cmp_entry);
    return k;
}

uint32_t radar_range_m(uint32_t max_dist_m) {
    static uint8_t const MANT[] = {1, 2, 5};
    for (uint64_t decade = 100; decade <= 10000000ull; decade *= 10) {
        for (size_t i = 0; i < sizeof(MANT); i++) {
            uint64_t r = decade * MANT[i];
            if (r >= max_dist_m) {
                return (uint32_t)r;
            }
        }
    }
    return 100000000u;
}

void radar_plot_xy(uint32_t dist_m, uint16_t bearing_deg, uint32_t range_m, int radius_px, int *x, int *y) {
    double f = range_m ? (double)dist_m / (double)range_m : 0.0;
    if (f > 1.0) {
        f = 1.0;
    }
    double r = f * radius_px, b = bearing_deg * DEG2RAD;
    *x = (int)lround(r * sin(b));
    *y = (int)lround(-r * cos(b));
}

void radar_format_dist(uint32_t dist_m, char *out, size_t cap) {
    if (dist_m < 10) {
        snprintf(out, cap, "<10 m");
    } else if (dist_m < 1000) {
        snprintf(out, cap, "%u m", (unsigned)dist_m);
    } else if (dist_m < 9950) {
        snprintf(out, cap, "%u.%u km", (unsigned)(dist_m + 50) / 1000, (unsigned)((dist_m + 50) % 1000) / 100);
    } else {
        snprintf(out, cap, "%u km", (unsigned)((dist_m + 500) / 1000));
    }
}

char const *radar_compass(uint16_t bearing_deg) {
    static char const *const N[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return N[((bearing_deg % 360) + 22) / 45 % 8];
}
