// Host test: when a radio change is confirmed first (#33)
#include "radio_confirm.h"
#include "radio_presets.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static radio_confirm_t between(size_t from, size_t to) {
    radio_preset_t const *a = radio_preset_at(from), *b = radio_preset_at(to);
    return radio_confirm_needed(&(radio_confirm_cfg_t){a->freq_mhz, a->bw_khz, a->sf, a->cr, 0}, &(radio_confirm_cfg_t){b->freq_mhz, b->bw_khz, b->sf, b->cr, 0}); // the path hash size is not part of a preset (#129)
}

int main(void) {
    CHECK(radio_band(869.618f) == 0);
    CHECK(radio_band(862.0f) == 0);
    CHECK(radio_band(910.525f) == 1);
    CHECK(radio_band(927.875f) == 1);
    CHECK(between(0, 0) == RADIO_CONFIRM_NONE);
    CHECK(between(0, 1) == RADIO_CONFIRM_SWITCH);  // Netherlands -> Limburg
    CHECK(between(0, 5) == RADIO_CONFIRM_NONE);    // Netherlands -> Hungary: the same radio
    CHECK(between(0, 23) == RADIO_CONFIRM_BAND);   // Netherlands -> Vietnam (Deprecated), the #33 incident
    CHECK(between(9, 0) == RADIO_CONFIRM_BAND);    // USA -> Netherlands
    CHECK(between(9, 14) == RADIO_CONFIRM_SWITCH); // USA -> Australia
    // Custom values equal to the current radio need no confirm
    radio_confirm_cfg_t const cur = {869.618f, 62.5f, 7, 5, 1}, same = {869.618f, 62.5f, 7, 5, 1},
                              hash3 = {869.618f, 62.5f, 7, 5, 3}, us = {915.0f, 62.5f, 7, 5, 1};
    CHECK(radio_confirm_needed(&cur, &same) == RADIO_CONFIRM_NONE);
    CHECK(radio_confirm_needed(&cur, &hash3) == RADIO_CONFIRM_SWITCH);
    CHECK(radio_confirm_needed(&cur, &us) == RADIO_CONFIRM_BAND);
    printf("radio_confirm_test: all checks passed\n");
    return 0;
}
