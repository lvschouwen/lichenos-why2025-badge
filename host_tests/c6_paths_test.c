// Host test for where the lean firmware (#61) keeps the C6 image files
#include "c6_paths.h"

#include <stdint.h>
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

// #420: the update server refuses writes and deletes in the C6 image folder while the on-air list holds; no spelling
// of the path gets around it, and nothing outside it is caught
static void image_dir_held(void) {
    CHECK(c6_path_in_image_dir("/storage/c6/firmware.bin"));
    CHECK(c6_path_in_image_dir("/storage/c6"));
    CHECK(c6_path_in_image_dir("/storage/c6/"));
    CHECK(c6_path_in_image_dir("//storage///c6//firmware.bin"));
    CHECK(c6_path_in_image_dir("/STORAGE/C6/firmware.bin"));
    CHECK(c6_path_in_image_dir("/storage/C6/sub/x"));
    CHECK(c6_path_in_image_dir("/storage/x/../c6/firmware.bin"));
    CHECK(c6_path_in_image_dir("/storage/./c6/firmware.bin"));
    CHECK(c6_path_in_image_dir("/storage/notes/.."));
    CHECK(!c6_path_in_image_dir("/storage/c6x/firmware.bin"));
    CHECK(!c6_path_in_image_dir("/storage/c/6"));
    CHECK(!c6_path_in_image_dir("/storage/notes.txt"));
    CHECK(!c6_path_in_image_dir("/storage/lichen/c6/firmware.bin"));
    CHECK(!c6_path_in_image_dir("/storage"));
    CHECK(!c6_path_in_image_dir("/sd/c6/firmware.bin"));
    CHECK(!c6_path_in_image_dir("/storage/..c6/x"));
    CHECK(!c6_path_in_image_dir(""));
}

int main(void) {
    char out[64];
    image_dir_held();

    CHECK(c6_image_path("firmware.bin", out, sizeof(out)) && strcmp(out, "/storage/c6/firmware.bin") == 0);
    CHECK(c6_image_path("firmware.bin.md5", out, sizeof(out)) && strcmp(out, "/storage/c6/firmware.bin.md5") == 0);

    // Only plain names inside the C6 folder
    CHECK(!c6_image_path("../x", out, sizeof(out)));
    CHECK(!c6_image_path("a/b", out, sizeof(out)));
    CHECK(!c6_image_path("", out, sizeof(out)));
    CHECK(!c6_image_path("..", out, sizeof(out)));

    // Never a truncated path
    CHECK(!c6_image_path("firmware.bin", out, 4));

    // The .md5 file next to an image must describe that image: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72
    uint8_t const abc[16] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
                             0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72};
    CHECK(c6_md5_matches("900150983cd24fb0d6963f7d28e17f72", abc));
    CHECK(c6_md5_matches("900150983CD24FB0D6963F7D28E17F72", abc));
    CHECK(!c6_md5_matches("900150983cd24fb0d6963f7d28e17f73", abc));
    CHECK(!c6_md5_matches("900150983cd24fb0d6963f7d28e17f7", abc)); // too short
    CHECK(!c6_md5_matches("", abc));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("c6_paths_test: all checks passed\n");
    return 0;
}
