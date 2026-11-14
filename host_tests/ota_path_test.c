// Host test: badge.sh's VMS-style paths mapped onto the lean firmware's drive (#61)
#include "ota_path.h"

#include "meshcore_app_record.h"
#include "ota_proto.h"

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

// The hold's answer for a push or delete of vms as badge.sh names it: what refused_on_hold decides (list on)
static ota_hold_t held_kind(char const *vms) {
    char posix[OTA_PATH_MAX + 16];
    if (!ota_path_to_posix(vms, posix, sizeof(posix))) {
        return OTA_HOLD_NONE; // refused as a bad path before any hold
    }
    return ota_route_usb_held(OTA_ROUTE_FILE, false, ota_path_onair_held(posix));
}

// #454: with the list on, a Wi-Fi push or delete of lora.toml, MeshCore's files or the token file is refused like
// the C6 folder, however the path is spelled; everything else stays as it was
static void test_onair_held(void) {
    char const *const held[] = {
        "FLASH0:lora.toml", "FLASH0:LORA.TOML", "FLASH0:Lora.Toml", "FLASH0:lora.toml.", "FLASH0:lora.toml..",
        "FLASH0:lora.toml.bak", "FLASH0:lora", "FLASH0:ota_token.txt", "FLASH0:OTA_TOKEN.TXT", "FLASH0:ota_token.txt.",
        "FLASH0:ota_token.tmp", "FLASH0:[meshcore]identity", "FLASH0:[MESHCORE]identity", "FLASH0:[MeshCore.identity]x",
        "FLASH0:[meshcore.identity]key.bin", "FLASH0:meshcore", "FLASH0:MeshCore", "FLASH0:[lora]x",
        "FLASH0:[ota_x.y]z",
    };
    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        if (held_kind(held[i]) != OTA_HOLD_FILE) {
            printf("FAIL not held: %s\n", held[i]);
            failures++;
        }
    }
    char const *const free_to_go[] = {
        "FLASH0:notes.txt", "FLASH0:[apps.hello]app.elf", "FLASH0:[meshcorex]x", "FLASH0:meshcore2",
        "FLASH0:[apps.meshcore]x", "FLASH0:[x]lora.toml", "FLASH0:[apps]ota_token.txt", "FLASH0:slora.toml",
    };
    for (size_t i = 0; i < sizeof(free_to_go) / sizeof(free_to_go[0]); i++) {
        if (held_kind(free_to_go[i]) != OTA_HOLD_NONE) {
            printf("FAIL held: %s\n", free_to_go[i]);
            failures++;
        }
    }
    // The posix form the server checks: odd segments and other spellings fail closed
    CHECK(ota_path_onair_held("//storage///lora.toml"));
    CHECK(ota_path_onair_held("/STORAGE/meshcore/x"));
    CHECK(ota_path_onair_held("/storage/lora.toml "));
    CHECK(ota_path_onair_held("/storage/meshcore. /x"));
    CHECK(ota_path_onair_held("/storage/x/../lora.toml"));
    CHECK(ota_path_onair_held("/storage/./notes.txt"));
    CHECK(ota_path_onair_held("/sd/notes.txt"));
    CHECK(!ota_path_onair_held("/storage/notes.txt"));
    CHECK(!ota_path_onair_held("/storage"));
    CHECK(!ota_path_onair_held(""));
    // The names agree with their owners: MeshCore's app root and the token file
    CHECK(ota_path_onair_held("/storage/" MESHCORE_STORAGE_ROOT "/identity"));
    char token_file[OTA_PATH_MAX + 16];
    CHECK(ota_path_to_posix("FLASH0:ota_token.txt", token_file, sizeof(token_file)) && ota_path_onair_held(token_file));
    // A file the hold does not cover keeps its route's answer: none (the list off asks nothing at all)
    CHECK(ota_route_usb_held(OTA_ROUTE_DELETE, false, false) == OTA_HOLD_NONE);
}

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

    test_onair_held();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_path_test: all checks passed\n");
    return 0;
}
