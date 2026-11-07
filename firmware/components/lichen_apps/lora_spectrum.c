// The LoRa spectrum app's sums (#161); see lora_spectrum.h
#include "lora_spectrum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

spec_band_t const spec_bands[SPEC_BANDS] = {
    {"868-870 MHz", 868000000, 870000000},
    {"863-870 MHz", 863000000, 870000000},
};

bool spec_plan(spec_band_t const *band, uint32_t channel_hz, uint32_t bw_hz, uint8_t max_bins, spec_plan_t *out) {
    memset(out, 0, sizeof(*out)); // padding too: a copy compares the same however it is compared
    if (!bw_hz || !max_bins || band->end_hz <= band->start_hz) {
        return false;
    }
    uint32_t span = band->end_hz - band->start_hz;
    uint32_t step = bw_hz;
    while (span / step > max_bins) {
        step *= 2;
    }
    if (step > span) {
        return false;
    }
    // A bin is centred on its frequency and must lie inside the band: centres from start + step/2 to end - step/2
    uint32_t lo     = band->start_hz + step / 2;
    uint32_t hi     = band->end_hz - step / 2;
    bool     inside = channel_hz >= lo && channel_hz <= hi;
    uint32_t anchor = inside ? channel_hz : lo;
    uint32_t below  = (anchor - lo) / step; // whole steps between the lowest centre and the anchor
    out->first_hz   = anchor - below * step;
    uint32_t n      = (hi - out->first_hz) / step + 1;
    out->count      = (uint8_t)(n > max_bins ? max_bins : n);
    out->step_hz    = step;
    out->channel_bin = inside && below < out->count ? (int)below : -1;
    return true;
}

bool spec_plan_equal(spec_plan_t const *a, spec_plan_t const *b) {
    return a->first_hz == b->first_hz && a->step_hz == b->step_hz && a->count == b->count &&
           a->channel_bin == b->channel_bin;
}

int spec_bar_px(int16_t rssi_x10, int height_px) {
    int32_t const floor_x10 = SPEC_FLOOR_DBM * 10, top_x10 = SPEC_TOP_DBM * 10;
    int32_t       v         = rssi_x10 < floor_x10 ? floor_x10 : rssi_x10 > top_x10 ? top_x10 : rssi_x10;
    int           h         = (int)((v - floor_x10) * height_px / (top_x10 - floor_x10));
    return h < SPEC_MIN_BAR ? SPEC_MIN_BAR : h;
}

void spec_bin_span(uint8_t i, uint8_t count, int width_px, int *x, int *w) {
    if (!count) {
        *x = 0, *w = 0;
        return;
    }
    int left  = i * width_px / count;
    int right = (i + 1) * width_px / count;
    *x        = left;
    *w        = right - left > 3 ? right - left - 1 : right - left;
}

void spec_peak_reset(int16_t *peak, size_t n) {
    for (size_t i = 0; i < n; i++) {
        peak[i] = INT16_MIN;
    }
}

void spec_peak_update(int16_t *peak, int16_t const *now, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (now[i] > peak[i]) {
            peak[i] = now[i];
        }
    }
}

size_t spec_strongest(int16_t const *rssi_x10, size_t n) {
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        if (rssi_x10[i] > rssi_x10[best]) {
            best = i;
        }
    }
    return best;
}

void spec_fmt_mhz(uint32_t hz, char *out, size_t cap) {
    uint32_t khz = (hz + 500) / 1000;
    snprintf(out, cap, "%lu.%03lu MHz", (unsigned long)(khz / 1000), (unsigned long)(khz % 1000));
}

void spec_fmt_dbm(int16_t rssi_x10, char *out, size_t cap) {
    int v = rssi_x10;
    snprintf(out, cap, "%s%d.%d dBm", v < 0 ? "-" : "", abs(v) / 10, abs(v) % 10);
}

void spec_fmt_khz(uint32_t hz, char *out, size_t cap) {
    if (hz % 1000 == 0) {
        snprintf(out, cap, "%lu kHz", (unsigned long)(hz / 1000));
    } else {
        snprintf(out, cap, "%lu.%lu kHz", (unsigned long)(hz / 1000), (unsigned long)(hz % 1000 / 100));
    }
}
