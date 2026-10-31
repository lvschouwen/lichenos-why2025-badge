// Host test: per-app settings rules (#84 T5): the NVS namespace from an app id, and adopting a legacy key
#include "app_ns.h"

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
    char ns[16];
    CHECK(app_ns_name("meshcore", ns, sizeof(ns)) && strcmp(ns, "a.meshcore") == 0);
    CHECK(app_ns_name("k_1", ns, sizeof(ns)) && strcmp(ns, "a.k_1") == 0);
    CHECK(app_ns_name("abcdefghijklm", ns, sizeof(ns)) && strlen(ns) == 15); // 13 characters: the longest id
    strcpy(ns, "x");
    CHECK(!app_ns_name("abcdefghijklmn", ns, sizeof(ns)) && strcmp(ns, "x") == 0); // 14: too long, out untouched
    CHECK(!app_ns_name("", ns, sizeof(ns)));
    CHECK(!app_ns_name(NULL, ns, sizeof(ns)));
    CHECK(!app_ns_name("Mesh", ns, sizeof(ns)));  // no capitals
    CHECK(!app_ns_name("a-b", ns, sizeof(ns)));   // no dashes
    CHECK(!app_ns_name("a.b", ns, sizeof(ns)));   // no dots
    CHECK(!app_ns_name("meshcore", ns, 15));      // a buffer that cannot hold the longest name

    CHECK(app_adopt_plan(false, true) == APP_ADOPT_COPY);  // only the old key: copy it once
    CHECK(app_adopt_plan(true, true) == APP_ADOPT_NONE);   // already adopted: the old key never wins again
    CHECK(app_adopt_plan(true, false) == APP_ADOPT_NONE);
    CHECK(app_adopt_plan(false, false) == APP_ADOPT_NONE); // a fresh badge: nothing to adopt

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
