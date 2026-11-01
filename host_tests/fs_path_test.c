// Host test: an app's directory paths on the storage drive and the SD card (#84 T6, #60)
#include "fs_path.h"

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
    char p[64];
    CHECK(fs_app_dir_path("/storage", "meshcore", p, sizeof(p)) && strcmp(p, "/storage/meshcore") == 0);
    CHECK(fs_app_dir_path("/sd/lichen/apps", "meshcore", p, sizeof(p)) && strcmp(p, "/sd/lichen/apps/meshcore") == 0);
    CHECK(fs_app_dir_path("/sd/lichen/apps", "abcdefghijklm", p, sizeof(p)) &&
          strcmp(p, "/sd/lichen/apps/abcdefghijklm") == 0); // the longest id

    // Ids that could leave the app's directory or collide are refused, and the buffer stays untouched
    strcpy(p, "x");
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "..", p, sizeof(p)) && strcmp(p, "x") == 0);
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "a/b", p, sizeof(p)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", ".", p, sizeof(p)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "", p, sizeof(p)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", NULL, p, sizeof(p)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "Mesh", p, sizeof(p))); // FAT folds case: two apps could meet
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "abcdefghijklmn", p, sizeof(p)));
    CHECK(!fs_app_dir_path("sd/apps", "meshcore", p, sizeof(p))); // relative base
    CHECK(!fs_app_dir_path(NULL, "meshcore", p, sizeof(p)));
    CHECK(strcmp(p, "x") == 0);

    // Exactly big enough, and one byte short
    char exact[sizeof("/sd/lichen/apps/meshcore")];
    CHECK(fs_app_dir_path("/sd/lichen/apps", "meshcore", exact, sizeof(exact)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "meshcore", exact, sizeof(exact) - 1));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "meshcore", NULL, 64));

    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
