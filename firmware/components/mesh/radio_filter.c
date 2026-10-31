// Type-to-search over the region presets (#32): see radio_filter.h
#include "radio_filter.h"

#include "list_filter.h"
#include "radio_presets.h"

#include <stdio.h>

// The EU countries that have no preset of their own (the Netherlands, Czechia, Hungary, Portugal and Slovakia do),
// plus the UK and the EEA/EFTA states MeshCore's EU/UK 869 MHz settings are used in
#define EU_UK                                                                                                          \
    "EU Europe UK GB Britain England Scotland Wales Ireland Austria Belgium Bulgaria Croatia Cyprus Denmark Estonia "   \
    "Finland France Germany Deutschland Greece Italy Latvia Lithuania Luxembourg Malta Poland Romania Slovenia Spain "  \
    "Sweden EEA Norway Iceland Liechtenstein"

// By preset index, in radio_presets.c order. Matched anywhere in the word, so no two-letter codes that would hit
// inside other words ("ca" is in "America")
static char const *const aliases[] = {
    "NL Holland Nederland Dutch",                           // Netherlands
    "NL Holland Nederland Dutch Limburg",                   // Netherlands (Limburg)
    EU_UK,                                                  // EU/UK (Narrow)
    EU_UK,                                                  // EU/UK (Deprecated)
    "CZ Czechia Cesko",                                     // Czech Republic (Narrow)
    "HU Magyarorszag",                                      // Hungary
    "PT",                                                   // Portugal 868
    "SK Slovensko",                                         // Slovakia
    "CH Swiss Schweiz Suisse Svizzera",                     // Switzerland
    "US America United States",                             // USA
    "SoCal California",                                     // USA - Southern California
    "CA",                                                   // Canada
    "CR",                                                   // Costa Rica
    "BR Brasil",                                            // Brazil
    "AU Oz",                                                // Australia
    "AU Oz",                                                // Australia (Narrow)
    "AU Oz",                                                // Australia (Mid)
    "AU Oz Adelaide Perth",                                 // Australia: SA, WA
    "AU Oz Queensland Brisbane",                            // Australia: QLD
    "NZ Aotearoa",                                          // New Zealand (Narrow)
    "NZ Aotearoa",                                          // New Zealand (Gisborne)
    "TH",                                                   // Thailand
    "VN Viet Nam",                                          // Vietnam (Narrow)
    "VN Viet Nam",                                          // Vietnam (Deprecated)
};

size_t radio_filter_alias_count(void) {
    return sizeof(aliases) / sizeof(aliases[0]);
}

typedef struct {
    radio_preset_t const *preset;
    char const           *aliases;
} ctx_t;

static bool word_ok(char const *word, void *arg) {
    ctx_t const *c = arg;
    if (list_filter_text_has(c->preset->title, word) || list_filter_text_has(c->aliases, word)) {
        return true;
    }
    if (list_filter_is_number(word)) {
        char freq[16];
        snprintf(freq, sizeof(freq), "%.3f", (double)c->preset->freq_mhz);
        return list_filter_text_starts(freq, word);
    }
    return false;
}

bool radio_filter_match(size_t preset, char const *query) {
    radio_preset_t const *p = radio_preset_at(preset);
    if (!p || preset >= radio_filter_alias_count()) {
        return false;
    }
    ctx_t c = {p, aliases[preset]};
    return list_filter_match(query, word_ok, &c);
}
