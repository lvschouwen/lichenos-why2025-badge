// Host test: per-app settings rules (#84 T5): the NVS namespace from an app id
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
    CHECK(app_ns_owns("meshcore", "a.meshcore") && !app_ns_owns("meshcore", "a.other") && !app_ns_owns("meshcore", "badge_fw"));
    CHECK(!app_ns_owns("Bad", "a.Bad") && !app_ns_owns("meshcore", NULL) && !app_ns_owns(NULL, "a.x"));
    CHECK(!app_ns_name("meshcore", ns, 15));      // a buffer that cannot hold the longest name

    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
