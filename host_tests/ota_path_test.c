// Host test: badge.sh's VMS-style paths mapped onto the lean firmware's drive (#61)
#include "ota_path.h"

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
    char out[96];

    CHECK(ota_path_to_posix("FLASH0:[c6]firmware.bin", out, sizeof(out)) &&
          strcmp(out, "/storage/c6/firmware.bin") == 0);
    CHECK(ota_path_to_posix("FLASH0:lora.toml", out, sizeof(out)) && strcmp(out, "/storage/lora.toml") == 0);
    CHECK(ota_path_to_posix("FLASH0:[A.B]c.txt", out, sizeof(out)) && strcmp(out, "/storage/A/B/c.txt") == 0);

    // Only the flash drive, and nothing that could leave it
    CHECK(!ota_path_to_posix("SD0:x", out, sizeof(out)));
    CHECK(!ota_path_to_posix("FLASH0:[..]x", out, sizeof(out)));
    CHECK(!ota_path_to_posix("FLASH0:[a..b]x", out, sizeof(out)));
    CHECK(!ota_path_to_posix("/etc/x", out, sizeof(out)));
    CHECK(!ota_path_to_posix("", out, sizeof(out)));

    // Never a truncated path
    CHECK(!ota_path_to_posix("FLASH0:lora.toml", out, 10));

    // The temporary file a push is written to first, next to its target
    CHECK(ota_temp_to_posix("FLASH0:lora.toml", out, sizeof(out)) && strcmp(out, "/storage/ota_upload.tmp") == 0);
    CHECK(ota_temp_to_posix("FLASH0:[c6]firmware.bin", out, sizeof(out)) &&
          strcmp(out, "/storage/c6/ota_upload.tmp") == 0);
    CHECK(!ota_temp_to_posix("SD0:x", out, sizeof(out)));
    CHECK(!ota_temp_to_posix("FLASH0:lora.toml", out, 12));

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_path_test: all checks passed\n");
    return 0;
}
