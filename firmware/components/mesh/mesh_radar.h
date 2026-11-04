#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Contact radar (#171): contacts that advertise a position, placed by distance and bearing from our own manual position
// (#52). Pure logic, no ESP headers; the screen is meshcore_radar.c.

#define RADAR_MAX    100  // contacts plotted and listed: the closest ones
#define RADAR_IN_MAX 1000 // contacts read from the table (the firmware's MAX_CONTACTS)
#define RADAR_NAME 33 // MESH_NAME_MAX + 1

// One contact as the mesh task hands it over (mesh_contacts_positions); 0/0 = it shares no position
typedef struct {
    uint8_t prefix[6];
    char    name[RADAR_NAME];
    uint8_t type;
    int32_t lat_e6, lon_e6;
} radar_contact_t;

typedef struct {
    uint8_t  prefix[6];
    char     name[RADAR_NAME];
    uint8_t  type;
    uint32_t dist_m;
    uint16_t bearing_deg; // 0 = north, clockwise, 0-359
    uint16_t src;         // index in the radar_build input (internal)
} radar_entry_t;

// Great-circle distance in metres (haversine, mean earth radius) and the initial bearing in degrees 0-359
double radar_distance_m(double lat1, double lon1, double lat2, double lon2);
double radar_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Entries for the contacts with a position, closest first (ties: name, then key). Returns how many; with more than max positioned contacts the max closest.
size_t radar_build(double own_lat, double own_lon, radar_contact_t const *in, size_t n, radar_entry_t *out, size_t max);

// The plot's outer ring: the smallest 1-2-5 step (100 m, 200 m, 500 m, 1 km, ...) holding max_dist_m; at least 100 m
uint32_t radar_range_m(uint32_t max_dist_m);
// Where an entry lands in a plot of the given pixel radius, relative to its centre (x right, y down, north up). A
// distance past range_m is held on the edge.
void radar_plot_xy(uint32_t dist_m, uint16_t bearing_deg, uint32_t range_m, int radius_px, int *x, int *y);

// "850 m", "1.2 km", "12 km", "230 km"; "<10 m" for anything nearer than 10 m
void radar_format_dist(uint32_t dist_m, char *out, size_t cap);
// 8-point compass name for a bearing: "N", "NE", ...
char const *radar_compass(uint16_t bearing_deg);

#ifdef __cplusplus
}
#endif
