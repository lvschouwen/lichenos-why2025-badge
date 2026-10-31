// Host test: MeshCore's region presets and the badge's radio limits (#67)
#include "radio_presets.h"

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

int main(void) {
    CHECK(radio_preset_count() == 24);
    radio_preset_t const *nl = radio_preset_at(0);
    CHECK(nl && strcmp(nl->title, "Netherlands") == 0);
    CHECK(nl && nl->sf == 7 && nl->cr == 5 && nl->path_hash_size == 1);
    CHECK(radio_preset_at(24) == NULL);

    radio_preset_t const *limburg = radio_preset_at(1);
    CHECK(limburg && strcmp(limburg->title, "Netherlands (Limburg)") == 0 && limburg->path_hash_size == 2);
    radio_preset_t const *last = radio_preset_at(23);
    CHECK(last && strcmp(last->title, "Vietnam (Deprecated)") == 0 && last->bw_khz == 250.0f && last->sf == 11);
    radio_preset_t const *socal = radio_preset_at(10);
    CHECK(socal && strcmp(socal->title, "USA - Southern California") == 0 && socal->path_hash_size == 3);

    CHECK(radio_preset_match(869.618f, 62.5f, 7, 5) == 0);
    CHECK(radio_preset_match(869.618f, 62.5f, 8, 8) == 1); // ties go to the first: Limburg before EU/UK (Narrow)
    CHECK(radio_preset_match(869.6182f, 62.5f, 7, 5) == 0);  // within 0.0005 MHz
    CHECK(radio_preset_match(868.0f, 62.5f, 7, 5) == -1);
    CHECK(radio_preset_match(869.618f, 125.0f, 7, 5) == -1);
    CHECK(radio_preset_match(915.8f, 250.0f, 10, 5) == 14); // Australia

    // With the path hash size: the exact entry, else Custom
    CHECK(radio_preset_match_exact(869.618f, 62.5f, 7, 5, 1) == 0);  // Netherlands
    CHECK(radio_preset_match_exact(869.618f, 62.5f, 7, 5, 2) == 5);  // Hungary (before Slovakia, the same values)
    CHECK(radio_preset_match_exact(869.618f, 62.5f, 7, 5, 3) == -1); // same radio, no entry with 3 bytes: Custom
    CHECK(radio_preset_match_exact(869.618f, 62.5f, 8, 8, 2) == 1);  // Limburg
    CHECK(radio_preset_match_exact(869.618f, 62.5f, 8, 8, 1) == 2);  // EU/UK (Narrow)
    CHECK(radio_preset_match_exact(927.875f, 62.5f, 7, 5, 3) == 10); // USA - Southern California
    CHECK(radio_preset_match_exact(868.0f, 62.5f, 7, 5, 1) == -1);
    for (size_t i = 0; i < radio_preset_count(); i++) {
        radio_preset_t const *p = radio_preset_at(i);
        int                   m = radio_preset_match_exact(p->freq_mhz, p->bw_khz, p->sf, p->cr, p->path_hash_size);
        CHECK(m >= 0 && m <= (int)i); // every entry finds itself or an identical one before it
    }

    CHECK(radio_bandwidth_count() == 10);
    CHECK(radio_bandwidth_at(0) == 7.8f && radio_bandwidth_at(6) == 62.5f && radio_bandwidth_at(9) == 500.0f);
    CHECK(radio_bandwidth_at(10) == 0.0f);
    for (size_t i = 0; i < radio_bandwidth_count(); i++) {
        CHECK(radio_check(869.618f, radio_bandwidth_at(i), 7, 5, 22) == NULL);
    }

    for (size_t i = 0; i < radio_preset_count(); i++) {
        radio_preset_t const *p = radio_preset_at(i);
        CHECK(p && p->freq_mhz >= 862.0f && p->freq_mhz <= 928.0f);
        CHECK(p && p->path_hash_size >= 1 && p->path_hash_size <= 3);
        CHECK(p && radio_check(p->freq_mhz, p->bw_khz, p->sf, p->cr, 22) == NULL);
    }

    // Review Focus 3: each limit refused with a reason, nothing else changed in the call
    CHECK(radio_check(850.0f, 62.5f, 7, 5, 22) != NULL);
    CHECK(radio_check(929.0f, 62.5f, 7, 5, 22) != NULL);
    CHECK(radio_check(869.618f, 300.0f, 7, 5, 22) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 13, 5, 22) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 4, 5, 22) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 7, 9, 22) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 7, 4, 22) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 7, 5, 30) != NULL);
    CHECK(radio_check(869.618f, 62.5f, 7, 5, -10) != NULL);
    CHECK(radio_check(0.0f / 0.0f, 62.5f, 7, 5, 22) != NULL); // NaN from a bad parse
    CHECK(radio_check(869.618f, 62.5f, 7, 5, 22) == NULL);
    CHECK(radio_check(862.0f, 7.8f, 5, 8, -9) == NULL);
    CHECK(radio_check(928.0f, 500.0f, 12, 5, 0) == NULL);
    CHECK(radio_check(869.618f, 31.25f, 7, 5, 22) == NULL);
    CHECK(radio_check(869.618f, 41.7f, 7, 5, 22) == NULL);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_presets_test: all checks passed\n");
    return 0;
}
