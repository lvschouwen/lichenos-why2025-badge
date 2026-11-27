// Host test: badge.sh's VMS-style paths mapped onto the lean firmware's drive (#61)
#include "ota_path.h"

#include "meshcore_app_record.h"
#include "ota_proto.h"

#include <errno.h>
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
    // #595 D3: the card's update folder installs at the next boot, so a push or delete there is held like ota-firmware;
    // every spelling of it, and the paths badge.sh sends, fail closed
    char upd[OTA_PATH_MAX + 16];
    CHECK(ota_path_to_posix("SD0:[lichen.update]badge_fw.bin", upd, sizeof(upd)) && ota_path_onair_held(upd));
    CHECK(ota_path_to_posix("SD0:[lichen.update.c6]firmware.bin.md5", upd, sizeof(upd)) && ota_path_onair_held(upd));
    CHECK(ota_path_onair_held("/sd/lichen/update/badge_fw.bin.sha256"));
    CHECK(ota_path_onair_held("/SD/Lichen/UPDATE/c6/firmware.bin"));
    CHECK(ota_path_onair_held("/sd/lichen/update./badge_fw.bin"));
    CHECK(ota_path_onair_held("//sd//lichen/x/../update/badge_fw.bin"));
    CHECK(ota_path_onair_held("/sd/lichen/update"));
    // Review M3: which Wi-Fi writes leave the update folder's Wi-Fi marker (fails closed: it only makes a trial stricter)
    CHECK(ota_path_to_posix("SD0:[lichen.update]badge_fw.bin", upd, sizeof(upd)) && ota_path_in_sd_update(upd));
    CHECK(ota_path_to_posix("SD0:[lichen.update.c6]firmware.bin", upd, sizeof(upd)) && ota_path_in_sd_update(upd));
    CHECK(ota_path_in_sd_update("/sd/lichen/update"));
    CHECK(ota_path_in_sd_update("/SD/Lichen/UPDATE./badge_fw.bin"));
    CHECK(ota_path_in_sd_update("//sd//lichen//update//badge_fw.bin.sha256"));
    CHECK(ota_path_in_sd_update("/sd/lichen/apps/../update/badge_fw.bin"));
    CHECK(ota_path_in_sd_update("/sd/./x"));
    // Security review: a Wi-Fi delete or overwrite of a marker-like name in the folder is a write there like any other
    CHECK(ota_path_in_sd_update("/sd/lichen/update/badge_fw.bin.wifi"));
    CHECK(ota_path_in_sd_update("/sd/lichen/update/BADGE_FW.BIN.WIFI. "));
    CHECK(!ota_path_in_sd_update("/sd/lichen/apps/x/app.elf"));
    CHECK(!ota_path_in_sd_update("/sd/lichen/updates/badge_fw.bin"));
    CHECK(!ota_path_in_sd_update("/sd/lichen"));
    CHECK(!ota_path_in_sd_update("/storage/lichen/update/badge_fw.bin"));
    CHECK(!ota_path_in_sd_update("/sd/badge_fw.bin"));
    CHECK(!ota_path_in_sd_update(""));
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

// #528: SD0:[dir.sub]name lands under /sd with the same name rules as FLASH0:
static void test_sd(void) {
    char out[96];
    CHECK(ota_path_to_posix("SD0:doom.wad", out, sizeof(out)) && strcmp(out, "/sd/doom.wad") == 0);
    CHECK(ota_path_to_posix("SD0:[apps.hello]app.elf", out, sizeof(out)) && strcmp(out, "/sd/apps/hello/app.elf") == 0);
    CHECK(ota_path_is_sd("SD0:x") && ota_path_is_sd("SD0:[a]x") && !ota_path_is_sd("FLASH0:x") && !ota_path_is_sd("SD1:x"));
    // The temp file sits next to the target, on the same drive
    CHECK(ota_temp_to_posix("SD0:doom.wad", out, sizeof(out)) && strcmp(out, "/sd/ota_upload.tmp") == 0);
    CHECK(ota_temp_to_posix("SD0:[apps.hello]app.elf", out, sizeof(out)) && strcmp(out, "/sd/apps/hello/ota_upload.tmp") == 0);
    // A streamed SD0: upload has its own temp name, next to the target, and reserved like the other (#529)
    CHECK(ota_stream_temp_to_posix("SD0:doom.wad", out, sizeof(out)) && strcmp(out, "/sd/ota_upload.tmp.sd") == 0);
    CHECK(ota_stream_temp_to_posix("SD0:[a.b]x.wad", out, sizeof(out)) && strcmp(out, "/sd/a/b/ota_upload.tmp.sd") == 0);
    CHECK(!ota_stream_temp_to_posix("SD1:x", out, sizeof(out)) && !ota_stream_temp_to_posix("SD0:x", out, 10));
    CHECK(!ota_path_allowed("SD0:ota_upload.tmp.sd") && !ota_path_allowed("SD0:[a]OTA_UPLOAD.TMP.SD"));
    // Refused like on FLASH0:
    char const *const bad[] = {"SD0:[..]x", "SD0:[a..b]x", "SD0:[.a]x", "SD0:[a.]x", "SD0:[]x", "SD0:", "SD0:[a]", "SD0:.hidden",
                               "SD0:ota_upload.tmp", "SD0:[a]ota_upload.tmp", "SD0:a/b", "SD0:[a/b]x", "sd0:x", "SD0:x y"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (ota_path_to_posix(bad[i], out, sizeof(out)) || ota_temp_to_posix(bad[i], out, sizeof(out))) {
            printf("FAIL accepted: %s\n", bad[i]);
            failures++;
        }
    }
    char longp[OTA_PATH_MAX + 16] = "SD0:";
    memset(longp + 4, 'a', OTA_PATH_MAX); // 4 + OTA_PATH_MAX characters: one drive prefix too many
    longp[4 + OTA_PATH_MAX] = '\0';
    CHECK(!ota_path_to_posix(longp, out, sizeof(out)));
    char big[OTA_PATH_MAX + 16];
    CHECK(!ota_path_to_posix("SD0:[apps.hello]app.elf", big, 10)); // never a truncated path
    CHECK(!ota_temp_to_posix("SD0:[apps.hello]app.elf", big, 20));
    // FAT names are case-blind: the temp name in any case is reserved too
    CHECK(!ota_path_allowed("SD0:OTA_UPLOAD.TMP") && !ota_path_allowed("FLASH0:[a]Ota_Upload.tmp"));
}

// #528: the decisions of an upload that need no hardware
static void test_upload_rules(void) {
    // The directories mkdir_parents creates: everything between the mount point and the file name
    size_t n = ota_path_next_dir("/sd/a/b/c.bin", 0);
    CHECK(n == 5);
    n = ota_path_next_dir("/sd/a/b/c.bin", n);
    CHECK(n == 7);
    CHECK(ota_path_next_dir("/sd/a/b/c.bin", n) == 0);
    CHECK(ota_path_next_dir("/sd/x", 0) == 0);       // the mount point is never made
    CHECK(ota_path_next_dir("/storage/x", 0) == 0);
    CHECK(ota_path_next_dir("/storage/a/x", 0) == 10);
    CHECK(ota_path_next_dir("/sd", 0) == 0 && ota_path_next_dir("", 0) == 0);
    // FAT refuses to rename over a file: SD0: retries after removing it, FLASH0: never
    CHECK(ota_rename_retry(true, EEXIST) && !ota_rename_retry(false, EEXIST) && !ota_rename_retry(true, EBUSY));
    // After the old file is gone the temp file is the only copy of the new data: it stays
    CHECK(!ota_place_drops_temp(OTA_PLACE_OK) && !ota_place_drops_temp(OTA_PLACE_LOST));
    CHECK(ota_place_drops_temp(OTA_PLACE_WRITE) && ota_place_drops_temp(OTA_PLACE_SWAP) && ota_place_drops_temp(OTA_PLACE_DIR));
    CHECK(ota_place_drops_temp(OTA_PLACE_VERIFY));
    // The card is needed and missing: 409; FLASH0: never asks
    CHECK(ota_sd_missing("SD0:x", false) && !ota_sd_missing("SD0:x", true) && !ota_sd_missing("FLASH0:x", false));
    // The room check rounds up to the drive's allocation unit
    CHECK(ota_cluster_bytes("SD0:x") > ota_cluster_bytes("FLASH0:x"));
    CHECK(!ota_file_fits(100000, 40000, ota_cluster_bytes("SD0:x")) && ota_file_fits(100000, 40000, ota_cluster_bytes("FLASH0:x")));
    // SD0: is held as a whole while the list is on, with its own refusal text
    CHECK(held_kind("SD0:[lichen.apps.x]app.elf") == OTA_HOLD_FILE && held_kind("SD0:doom.wad") == OTA_HOLD_FILE);
    char text[200];
    ota_hold_reply(OTA_HOLD_SD, "the list", text, sizeof(text));
    CHECK(strstr(text, "SD card") && strstr(text, "the list"));
    CHECK(ota_path_onair_held("/sd") && ota_path_onair_held("/SD/x"));
}

int main(void) {
    char out[96];

    CHECK(ota_path_to_posix("FLASH0:[c6]firmware.bin", out, sizeof(out)) &&
          strcmp(out, "/storage/c6/firmware.bin") == 0);
    CHECK(ota_path_to_posix("FLASH0:lora.toml", out, sizeof(out)) && strcmp(out, "/storage/lora.toml") == 0);
    CHECK(ota_path_to_posix("FLASH0:[A.B]c.txt", out, sizeof(out)) && strcmp(out, "/storage/A/B/c.txt") == 0);

    // Only FLASH0: and SD0:, and nothing that could leave them
    CHECK(!ota_path_to_posix("SD1:x", out, sizeof(out)));
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
    CHECK(!ota_temp_to_posix("SD1:x", out, sizeof(out)));
    CHECK(!ota_temp_to_posix("FLASH0:lora.toml", out, 12));

    test_sd();
    test_upload_rules();
    test_onair_held();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ota_path_test: all checks passed\n");
    return 0;
}
