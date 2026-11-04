// Host test: Contact radar maths, order and texts (#171)
#include "mesh_radar.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(got, want)                                                                                           \
    do {                                                                                                               \
        if (strcmp((got), (want)) != 0) {                                                                              \
            printf("FAIL %s:%d: got '%s' want '%s'\n", __FILE__, __LINE__, (got), (want));                             \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void test_distance(void) {
    CHECK(radar_distance_m(52.0, 4.0, 52.0, 4.0) < 0.001);
    // one degree of latitude is 111.195 km on the mean-radius sphere
    CHECK(fabs(radar_distance_m(52.0, 4.0, 53.0, 4.0) - 111195.0) < 50.0);
    // Amsterdam - Paris is about 430 km
    double d = radar_distance_m(52.3676, 4.9041, 48.8566, 2.3522);
    CHECK(d > 427000 && d < 433000);
    // symmetric, and across the antimeridian the short way
    CHECK(fabs(radar_distance_m(0, 179.5, 0, -179.5) - 111195.0) < 50.0);
}

static void test_bearing(void) {
    CHECK(fabs(radar_bearing_deg(52, 4, 53, 4) - 0.0) < 0.01);
    CHECK(fabs(radar_bearing_deg(0, 4, 0, 5) - 90.0) < 0.01);
    CHECK(fabs(radar_bearing_deg(52, 4, 51, 4) - 180.0) < 0.01);
    CHECK(fabs(radar_bearing_deg(0, 5, 0, 4) - 270.0) < 0.01);
    double b = radar_bearing_deg(52.3676, 4.9041, 48.8566, 2.3522); // Paris: south-south-west
    CHECK(b > 200 && b < 215);
}

static radar_contact_t contact(char const *name, int32_t lat_e6, int32_t lon_e6) {
    radar_contact_t c = {0};
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.prefix[0] = (uint8_t)name[0];
    c.lat_e6    = lat_e6;
    c.lon_e6    = lon_e6;
    return c;
}

static void test_build(void) {
    radar_contact_t in[5] = {
        contact("far", 52500000, 4000000),   // 55.6 km north
        contact("none", 0, 0),               // no position: left out
        contact("near", 52001000, 4000000),  // 111 m north
        contact("east", 52000000, 4010000),  // east, about 685 m
        contact("also", 52001000, 4000000),  // ties with "near": name order
    };
    radar_entry_t out[RADAR_MAX];
    size_t        n = radar_build(52.0, 4.0, in, 5, out, RADAR_MAX);
    CHECK(n == 4);
    CHECK_STR(out[0].name, "also");
    CHECK_STR(out[1].name, "near");
    CHECK_STR(out[2].name, "east");
    CHECK_STR(out[3].name, "far");
    CHECK(out[1].dist_m >= 110 && out[1].dist_m <= 112);
    CHECK(out[1].bearing_deg == 0);
    CHECK(out[2].bearing_deg == 90);
    CHECK(out[3].dist_m > 55000 && out[3].dist_m < 56000);
    CHECK(radar_build(52.0, 4.0, in, 5, out, 2) == 2); // capped: the two closest are kept
    CHECK_STR(out[0].name, "also");
    CHECK_STR(out[1].name, "near");
    CHECK(radar_build(52.0, 4.0, in, 0, out, 2) == 0);
}

static void test_bad_input(void) {
    radar_contact_t in[4] = {
        contact("lat", 200000000, 4000000),    // latitude out of range
        contact("lon", 52000000, -181000000),  // longitude out of range
        contact("ok", 52001000, 4000000),
        contact("neg", -90000001, 0),
    };
    radar_entry_t out[RADAR_MAX];
    size_t        n = radar_build(52.0, 4.0, in, 4, out, RADAR_MAX);
    CHECK(n == 1);
    CHECK_STR(out[0].name, "ok");
    CHECK(radar_range_m(out[0].dist_m) == 200); // the outliers do not set the range
    double nan = NAN;
    CHECK(radar_distance_m(nan, 4.0, 52.0, 4.0) == 0.0);
    CHECK(radar_distance_m(52.0, 4.0, INFINITY, 4.0) >= 0.0);
}

static void test_full_buffer(void) {
    // five contacts at the same distance (ties by name), room for three: the first three names stay
    radar_contact_t in[6] = {contact("e", 52001000, 4000000), contact("d", 52001000, 4000000),
                             contact("c", 52001000, 4000000), contact("b", 52001000, 4000000),
                             contact("a", 52001000, 4000000), contact("far", 53000000, 4000000)};
    radar_entry_t out[3];
    size_t        n = radar_build(52.0, 4.0, in, 6, out, 3);
    CHECK(n == 3);
    CHECK_STR(out[0].name, "a");
    CHECK_STR(out[1].name, "b");
    CHECK_STR(out[2].name, "c");
    CHECK(out[0].bearing_deg == 0); // bearing of the kept entries comes from the right source contact
}

static void test_poles(void) {
    CHECK(fabs(radar_distance_m(90, 0, -90, 0) - 20015000.0) < 1000.0);
    CHECK(fabs(radar_distance_m(90, 10, 90, 170)) < 1.0);
    CHECK(fabs(radar_bearing_deg(89, 0, 90, 0) - 0.0) < 0.01);
    double b = radar_bearing_deg(-89, 0, -90, 0);
    CHECK(b >= 0 && b < 360 && fabs(b - 180.0) < 0.01);
    CHECK(radar_bearing_deg(90, 0, 52, 4) >= 0.0);
}

static void test_range(void) {
    CHECK(radar_range_m(0) == 100);
    CHECK(radar_range_m(100) == 100);
    CHECK(radar_range_m(101) == 200);
    CHECK(radar_range_m(450) == 500);
    CHECK(radar_range_m(501) == 1000);
    CHECK(radar_range_m(55600) == 100000);
    CHECK(radar_range_m(2000000) == 2000000);
    CHECK(radar_range_m(0xFFFFFFFFu) >= 10000000u);
}

static void test_plot(void) {
    int x, y;
    radar_plot_xy(0, 123, 1000, 100, &x, &y);
    CHECK(x == 0 && y == 0);
    radar_plot_xy(500, 0, 1000, 100, &x, &y); // north is up
    CHECK(x == 0 && y == -50);
    radar_plot_xy(1000, 90, 1000, 100, &x, &y); // east is right
    CHECK(x == 100 && y == 0);
    radar_plot_xy(500, 180, 1000, 100, &x, &y);
    CHECK(x == 0 && y == 50);
    radar_plot_xy(500, 270, 1000, 100, &x, &y);
    CHECK(x == -50 && y == 0);
    radar_plot_xy(5000, 90, 1000, 100, &x, &y); // past the range: on the edge
    CHECK(x == 100 && y == 0);
    radar_plot_xy(5, 90, 0, 100, &x, &y); // no range: centre, no division by zero
    CHECK(x == 0 && y == 0);
}

static void test_format(void) {
    char t[24];
    radar_format_dist(0, t, sizeof(t));      CHECK_STR(t, "<10 m");
    radar_format_dist(9, t, sizeof(t));      CHECK_STR(t, "<10 m");
    radar_format_dist(10, t, sizeof(t));     CHECK_STR(t, "10 m");
    radar_format_dist(850, t, sizeof(t));    CHECK_STR(t, "850 m");
    radar_format_dist(999, t, sizeof(t));    CHECK_STR(t, "999 m");
    radar_format_dist(1000, t, sizeof(t));   CHECK_STR(t, "1.0 km");
    radar_format_dist(1249, t, sizeof(t));   CHECK_STR(t, "1.2 km");
    radar_format_dist(9940, t, sizeof(t));   CHECK_STR(t, "9.9 km");
    radar_format_dist(9950, t, sizeof(t));   CHECK_STR(t, "10 km");
    radar_format_dist(12400, t, sizeof(t));  CHECK_STR(t, "12 km");
    radar_format_dist(230000, t, sizeof(t)); CHECK_STR(t, "230 km");
    CHECK_STR(radar_compass(0), "N");
    CHECK_STR(radar_compass(22), "N");
    CHECK_STR(radar_compass(23), "NE");
    CHECK_STR(radar_compass(90), "E");
    CHECK_STR(radar_compass(225), "SW");
    CHECK_STR(radar_compass(338), "N");
    CHECK_STR(radar_compass(337), "NW");
}

int main(void) {
    test_distance();
    test_bearing();
    test_build();
    test_bad_input();
    test_full_buffer();
    test_poles();
    test_range();
    test_plot();
    test_format();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("mesh_radar_test: ok\n");
    return 0;
}
