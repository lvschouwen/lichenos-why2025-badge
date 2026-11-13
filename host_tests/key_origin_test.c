// Key origin (#391): only the TCA8418 is a hand. A "phys" injected key goes through the remote key gate but is still
// injected, so hand-only paths refuse it.
#include "key_origin.h"

#include <stdio.h>

static int failures;
#define CHECK(c)                                                                                                      \
    do {                                                                                                              \
        if (!(c)) {                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                       \
            failures++;                                                                                               \
        }                                                                                                             \
    } while (0)

int main(void) {
    CHECK(!key_origin_injected(KEY_ORIGIN_CHIP) && key_origin_gated(KEY_ORIGIN_CHIP));
    key_origin_t plain = key_origin_of_inject(false);
    CHECK(key_origin_injected(plain) && !key_origin_gated(plain));
    key_origin_t phys = key_origin_of_inject(true);
    CHECK(key_origin_injected(phys) && key_origin_gated(phys)); // gated like the chip, but not a hand
    CHECK(key_origin_injected((key_origin_t)7));                // unknown: never a hand
    if (failures) {
        printf("key_origin_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("key_origin_test: OK\n");
    return 0;
}
