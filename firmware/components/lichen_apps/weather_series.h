#pragma once
// Weather (#155): the pure part. A ring of one-minute environment samples (kept in PSRAM by weather_app.c), the series
// and axis range for the graph, the relative air-quality trend and the CSV log lines. No ESP-IDF here, host-tested
// (weather_series_test.c). A missing reading is NaN everywhere.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WX_TEMP, WX_HUM, WX_PRESS, WX_GAS, WX_QTY } wx_qty_t;

#define WX_RING_CAP  360 // one sample a minute: 6 h
#define WX_OFFSET_MAX_HALF 20 // the temperature offset is -10.0..0.0 K in half-kelvin steps

typedef struct {
    float v[WX_QTY]; // raw readings: the temperature offset is applied when shown or logged, so changing it fixes the graph
} wx_sample_t;

typedef struct {
    wx_sample_t *buf; // caller's storage of cap samples
    size_t       cap, head, count; // head = next slot to write
} wx_ring_t;

void   wx_ring_init(wx_ring_t *r, wx_sample_t *storage, size_t cap);
void   wx_ring_push(wx_ring_t *r, wx_sample_t const *s); // overwrites the oldest when full
// Quantity q of the samples, oldest first, up to max values; the temperature gets offset_k added. Returns the count.
size_t wx_series(wx_ring_t const *r, wx_qty_t q, float offset_k, float *out, size_t max);

// Smallest and largest of the valid values; false when there is none
bool wx_minmax(float const *v, size_t n, float *lo, float *hi);
// The graph's vertical range: the data's min/max, widened to at least a sensible span for the quantity (so sensor
// noise does not fill the graph) and 5% padding. False when there is no valid value.
bool wx_range(float const *v, size_t n, wx_qty_t q, float *lo, float *hi);
// The value as an integer for the chart (tenths); the same scale for every quantity
int32_t wx_chart_value(float v);

typedef enum { WX_AIR_LEARNING, WX_AIR_BETTER, WX_AIR_SAME, WX_AIR_WORSE } wx_air_t;
// Gas resistance (kOhm, higher = cleaner) as a trend: the newest few readings against the hours before. It is relative
// to this badge and this room, not an absolute index; LEARNING until there is enough history.
wx_air_t    wx_air_trend(float const *gas, size_t n);
char const *wx_air_text(wx_air_t t); // "Cleaner than usual"

// "21.5 \xC2\xB0""C", "45%", "1013 hPa", "123 kOhm"; "no reading" for NaN. Returns the length like snprintf.
size_t wx_fmt_value(char *out, size_t cap, wx_qty_t q, float v);
char const *wx_qty_name(wx_qty_t q); // "Temperature"
// How far back n one-minute samples reach: "now" for one or none, "45 min", "2 h 10 min", "6 h"
void wx_span_text(char *out, size_t cap, size_t n);

// Temperature offset in half kelvin: the stored byte is half + WX_OFFSET_MAX_HALF (0 = no offset = 20)
int  wx_offset_half_from_byte(uint8_t b); // clamped to -20..0
uint8_t wx_offset_byte(int half);
void wx_offset_text(char *out, size_t cap, int half); // "0 K", "-1.5 K"

// The CSV log (UTC time, offset-corrected temperature, empty field for no reading)
size_t wx_csv_header(char *out, size_t cap);
size_t wx_csv_line(char *out, size_t cap, int64_t unix_s, wx_sample_t const *s, float offset_k);

#ifdef __cplusplus
}
#endif
