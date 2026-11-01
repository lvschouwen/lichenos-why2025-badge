// Host test: the per-source notification switch (#62): default on, off drops (counted), on again lets through
#include "notify_mute.h"

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
    nmute_t t;
    memset(&t, 0, sizeof(t));

    // Default on: an unknown source is never dropped
    CHECK(nmute_get(&t, "meshcore") == NMUTE_UNKNOWN);
    CHECK(!nmute_drop(&t, "meshcore") && t.muted == 0);
    nmute_put(&t, "meshcore", true);
    CHECK(nmute_get(&t, "meshcore") == NMUTE_ON && !nmute_drop(&t, "meshcore"));

    // Off drops and counts; other sources are untouched
    nmute_put(&t, "meshcore", false);
    CHECK(nmute_get(&t, "meshcore") == NMUTE_OFF);
    CHECK(nmute_drop(&t, "meshcore") && nmute_drop(&t, "meshcore") && t.muted == 2);
    CHECK(!nmute_drop(&t, "lichen"));

    // On again lets notices through; the count stays as a record
    nmute_put(&t, "meshcore", true);
    CHECK(!nmute_drop(&t, "meshcore") && t.muted == 2);

    // NVS keys: "n.<source>", at most 15 characters
    char key[NMUTE_KEY_CAP];
    CHECK(nmute_key("meshcore", key, sizeof(key)) && strcmp(key, "n.meshcore") == 0);
    CHECK(nmute_key("thirteenchars", key, sizeof(key)) && strlen(key) == 15);
    CHECK(!nmute_key("fourteen_chars", key, sizeof(key)));
    CHECK(!nmute_key("", key, sizeof(key)) && !nmute_key(NULL, key, sizeof(key)));

    // A full cache forgets its oldest switch (it is read from NVS again), keeps the newest
    char name[8];
    for (int i = 0; i < NMUTE_SLOTS + 3; i++) {
        snprintf(name, sizeof(name), "s%d", i);
        nmute_put(&t, name, false);
    }
    CHECK(t.used == NMUTE_SLOTS && nmute_get(&t, "meshcore") == NMUTE_UNKNOWN);
    snprintf(name, sizeof(name), "s%d", NMUTE_SLOTS + 2);
    CHECK(nmute_get(&t, name) == NMUTE_OFF);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("notify_mute_test: ok\n");
    return 0;
}
