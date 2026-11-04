#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The LoRa spectrum app's sums (#161): which frequencies a sweep measures, where each lands on the graph, the peak hold
// and the texts. Pure, no ESP or LVGL headers (host_tests/lora_spectrum_test.c); app_lora_spectrum.c draws.

// The bands Enter switches between: the 868-870 MHz part around the MeshCore channel first (fast, a packet is easy to
// catch), then the whole 863-870 MHz SRD band
typedef struct {
    char const *name; // "868-870 MHz"
    uint32_t    start_hz;
    uint32_t    end_hz;
} spec_band_t;

#define SPEC_BANDS 2
extern spec_band_t const spec_bands[SPEC_BANDS];

// What to ask the C6 for: count steps step_hz apart from first_hz, each a bin centred on its frequency, together
// covering the band. The step is the channel's bandwidth (doubled until the steps fit max_bins), so one bin hears what
// the channel hears. The grid is laid on the channel, so the channel has a bin of its own (channel_bin, -1 when the
// channel is outside the band). False when nothing fits (zero bandwidth, a band narrower than one step).
typedef struct {
    uint32_t first_hz;
    uint32_t step_hz;
    uint8_t  count;
    int      channel_bin;
} spec_plan_t;

bool spec_plan(spec_band_t const *band, uint32_t channel_hz, uint32_t bw_hz, uint8_t max_bins, spec_plan_t *out);
// Field by field (the struct has padding, so never memcmp two plans)
bool spec_plan_equal(spec_plan_t const *a, spec_plan_t const *b);

// The graph's scale: SPEC_FLOOR_DBM at the bottom, SPEC_TOP_DBM at the top. The SX1262's noise floor with 62.5 kHz is
// about -120 dBm; a badge on the table is about -40.
#define SPEC_FLOOR_DBM (-130)
#define SPEC_TOP_DBM   (-40)
#define SPEC_MIN_BAR   2 // an empty bin still shows a line

// Bar height in pixels for a reading in dBm * 10, SPEC_MIN_BAR..height_px
int spec_bar_px(int16_t rssi_x10, int height_px);

// Bin i of count across width_px: its left edge and width (one pixel left between bins when they are wide enough)
void spec_bin_span(uint8_t i, uint8_t count, int width_px, int *x, int *w);

// Peak hold: each bin keeps the highest reading since the last reset
void spec_peak_reset(int16_t *peak, size_t n);
void spec_peak_update(int16_t *peak, int16_t const *now, size_t n);

// Index of the strongest reading, 0 for n == 0
size_t spec_strongest(int16_t const *rssi_x10, size_t n);

// "869.618 MHz" (three decimals, rounded to the kHz) and "-112.5 dBm"
void spec_fmt_mhz(uint32_t hz, char *out, size_t cap);
void spec_fmt_dbm(int16_t rssi_x10, char *out, size_t cap);
// "62.5 kHz", "125 kHz"
void spec_fmt_khz(uint32_t hz, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
