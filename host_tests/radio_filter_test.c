// Host test: type-to-search over the region presets (#32)
#include "radio_filter.h"
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

// How many presets match, and the first and last of them
static unsigned mask_count(char const *q, unsigned long *first, unsigned long *last) {
    unsigned n = 0;
    *first = *last = 0;
    for (size_t i = 0; i < radio_preset_count(); i++) {
        if (radio_filter_match(i, q)) {
            if (!n) {
                *first = i;
            }
            *last = i;
            n++;
        }
    }
    return n;
}

#define EXPECT(q, count, a, b)                                                                                         \
    do {                                                                                                               \
        unsigned long f_, l_;                                                                                          \
        unsigned      n_ = mask_count(q, &f_, &l_);                                                                    \
        if (n_ != (count) || (n_ && (f_ != (a) || l_ != (b)))) {                                                       \
            printf("FAIL %s:%d: \"%s\" -> %u matches, first %lu last %lu\n", __FILE__, __LINE__, q, n_, f_, l_);       \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

int main(void) {
    CHECK(radio_filter_alias_count() == radio_preset_count());
    EXPECT("", 24, 0, 23);
    EXPECT("   ", 24, 0, 23);
    EXPECT("neth", 2, 0, 1);
    EXPECT("NETH", 2, 0, 1);
    EXPECT("holland", 2, 0, 1);
    EXPECT("nederland", 2, 0, 1);
    EXPECT("lim", 1, 1, 1);
    EXPECT("xyz", 0, 0, 0);
    EXPECT("aus", 7, 2, 18); // the five Australia rows, and EU/UK (Austria)
    EXPECT("aus narrow", 2, 2, 15); // Australia (Narrow), EU/UK (Narrow)
    EXPECT("oz", 5, 14, 18);
    EXPECT("nz gis", 1, 20, 20);
    EXPECT("swiss", 1, 8, 8);
    EXPECT("schweiz", 1, 8, 8);
    EXPECT("czech", 1, 4, 4);
    EXPECT("brasil", 1, 13, 13);
    EXPECT("costa", 1, 12, 12);
    EXPECT("viet", 2, 22, 23);
    EXPECT("socal", 1, 10, 10);
    EXPECT("california", 1, 10, 10);
    // every EU country (those with a preset of their own show that one), the UK and the EFTA states: the EU/UK rows
    EXPECT("germany", 2, 2, 3);
    EXPECT("france", 2, 2, 3);
    EXPECT("belgium", 2, 2, 3);
    EXPECT("poland", 2, 2, 3);
    EXPECT("sweden", 2, 2, 3);
    EXPECT("finland", 2, 2, 3);
    EXPECT("britain", 2, 2, 3);
    EXPECT("scotland", 2, 2, 3);
    EXPECT("norway", 2, 2, 3);
    EXPECT("eu", 2, 2, 3);
    EXPECT("hungary", 1, 5, 5);
    EXPECT("slovakia", 1, 7, 7);
    EXPECT("slovenia", 2, 2, 3);
    EXPECT("portugal", 1, 6, 6);
    // matched anywhere in a word, as chosen: "nl" is inside "Finland" too
    EXPECT("nl", 4, 0, 3);
    // frequencies by prefix
    EXPECT("869.618", 7, 0, 8);   // NL x2, EU/UK Narrow, Hungary, Portugal, Slovakia, Switzerland
    EXPECT("915", 2, 14, 16);     // Australia, Australia (Mid)
    EXPECT("923.1", 3, 13, 18);   // Brazil, SA/WA, QLD
    EXPECT("869.6 ne", 2, 0, 1);  // a number and a word
    EXPECT("1", 0, 0, 0);         // no frequency starts with 1
    CHECK(!radio_filter_match(24, ""));
    CHECK(!radio_filter_match(0, "zzz"));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("radio_filter_test ok\n");
    return 0;
}
