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

    // The OS's own directories on the drive are no app's (#283: the C6 image, the notification store)
    CHECK(!fs_app_dir_path("/storage", "c6", p, sizeof(p)) && strcmp(p, "x") == 0);
    CHECK(!fs_app_dir_path("/storage", "lichen", p, sizeof(p)) && !fs_app_dir_path("/sd/lichen/apps", "c6", p, sizeof(p)));
    CHECK(!fs_app_dir_path("/storage", "crash", p, sizeof(p))); // the crash log (#294)
    CHECK(fs_app_dir_path("/storage", "c", p, sizeof(p)) && fs_app_dir_path("/storage", "c6x", p, sizeof(p)) &&
          fs_app_dir_path("/storage", "lichenx", p, sizeof(p)));

    // Exactly big enough, and one byte short
    char exact[sizeof("/sd/lichen/apps/meshcore")];
    CHECK(fs_app_dir_path("/sd/lichen/apps", "meshcore", exact, sizeof(exact)));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "meshcore", exact, sizeof(exact) - 1));
    CHECK(!fs_app_dir_path("/sd/lichen/apps", "meshcore", NULL, 64));

    // The sandbox of an SD app's file calls (#533)
    char const *D = "/sd/lichen/apps/hello";
    char       r[128];
    CHECK(fs_sandbox_resolve(D, "wad/doom.wad", false, r, sizeof(r)) && strcmp(r, "/sd/lichen/apps/hello/wad/doom.wad") == 0);
    CHECK(fs_sandbox_resolve(D, "/sd/lichen/apps/hello/save.dat", false, r, sizeof(r)) &&
          strcmp(r, "/sd/lichen/apps/hello/save.dat") == 0);
    CHECK(fs_sandbox_resolve(D, "./a//b/./c", false, r, sizeof(r)) && strcmp(r, "/sd/lichen/apps/hello/a/b/c") == 0);
    CHECK(fs_sandbox_resolve(D, "/sd/lichen/apps/hello/", true, r, sizeof(r)) && strcmp(r, D) == 0); // dir itself, mkdir only
    CHECK(fs_sandbox_resolve(D, ".", true, r, sizeof(r)) && strcmp(r, D) == 0);
    CHECK(!fs_sandbox_resolve(D, ".", false, r, sizeof(r)) && r[0] == '\0'); // open, remove, rename: below the folder
    CHECK(!fs_sandbox_resolve(D, "/sd/lichen/apps/hello", false, r, sizeof(r)));
    // Escapes
    CHECK(!fs_sandbox_resolve(D, "", true, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "..", true, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "../other/x", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "a/../../x", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "a/..", false, r, sizeof(r))); // not even when it stays inside: no ".." at all
    CHECK(!fs_sandbox_resolve(D, "/sd/lichen/apps/hello/../other/x", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, ".. ", false, r, sizeof(r)));  // FatFs strips the space: it is ".."
    CHECK(!fs_sandbox_resolve(D, "...", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "a./b", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "a\\..\\..\\x", false, r, sizeof(r))); // backslash is a separator to FatFs
    CHECK(!fs_sandbox_resolve(D, "1:/x", false, r, sizeof(r)));        // a drive
    CHECK(!fs_sandbox_resolve(D, "/storage/lora.toml", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "/sd/lichen/apps/other/x", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "/sd/lichen/apps/hello2/x", false, r, sizeof(r))); // shares the prefix only
    CHECK(!fs_sandbox_resolve(D, "/SD/lichen/apps/hello/x", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "/", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "a\nb", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve("sd/x", "a", false, r, sizeof(r))); // a dir that is not absolute
    CHECK(!fs_sandbox_resolve("/sd/x/", "a", false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(NULL, "a", false, r, sizeof(r)) && !fs_sandbox_resolve(D, NULL, false, r, sizeof(r)));
    // Overlong: refused, never cut
    char longp[300];
    memset(longp, 'a', sizeof(longp) - 1);
    longp[sizeof(longp) - 1] = '\0';
    CHECK(!fs_sandbox_resolve(D, longp, false, r, sizeof(r)));
    CHECK(!fs_sandbox_resolve(D, "0123456789/0123456789/0123456789/0123456789/0123456789/0123456789", false, r, 40));
    char tight[sizeof("/sd/lichen/apps/hello/abc")];
    CHECK(fs_sandbox_resolve(D, "abc", false, tight, sizeof(tight)));
    CHECK(!fs_sandbox_resolve(D, "abcd", false, tight, sizeof(tight)) && tight[0] == '\0');
    CHECK(!fs_sandbox_resolve(D, "a", false, r, 0));

    CHECK(fs_sandbox_within("/d/a", "/d/a") && fs_sandbox_within("/d/a", "/d/a/b/c"));
    CHECK(!fs_sandbox_within("/d/a", "/d/ab") && !fs_sandbox_within("/d/a/b", "/d/a") && !fs_sandbox_within(NULL, "/d"));
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
