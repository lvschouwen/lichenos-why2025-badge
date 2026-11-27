// The C6 bundle on the SD card (#595): see c6_card.h; the rules are c6_boot_policy.c's (host-tested)
#include "c6_card.h"

#include "c6_boot_policy.h"
#include "c6_firmware.h"
#include "c6_hooks_priv.h"
#include "c6_paths.h"
#include "fw_console.h"
#include "nvs_fail.h"
#include "sdcard.h"
#include "storage.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_md5.h"
#include "nvs.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char const TAG[]        = "c6_card";
static char const KEY_FAILED[] = "c6_card_bad"; // FW_NVS_NAMESPACE: c6_card_record of the last card bundle that failed
static char const *const NAMES[3] = {"bootloader.bin", "partitions.bin", "firmware.bin"};

#define CARD_WAIT_MS 2000 // nothing else uses the card this early in the boot
#define COPY_PIECE   4096

static struct {
    bool from_card; // the flasher's source this boot
    bool locked;    // the card's batch lock is held (c6_card_begin to c6_card_end_reading)
    char key[C6_BUNDLE_KEY_MAX];
} s;

// One .bin of dir: its size and the .md5 text; *present counts the two files that exist
static bool bundle_file(char const *dir, char const *name, char md5[40], unsigned long *size, unsigned *present) {
    char path[80], md5_name[40];
    snprintf(md5_name, sizeof(md5_name), "%s.md5", name);
    struct stat st;
    bool        bin_ok = c6_image_path_in(dir, name, path, sizeof(path)) && stat(path, &st) == 0 && st.st_size > 0;
    *size              = bin_ok ? (unsigned long)st.st_size : 0;
    FILE *f            = c6_image_path_in(dir, md5_name, path, sizeof(path)) ? fopen(path, "r") : NULL;
    size_t n           = 0;
    if (f) {
        n = fread(md5, 1, 39, f);
        fclose(f);
    }
    md5[n] = '\0';
    *present += (unsigned)bin_ok + (f != NULL);
    return bin_ok && n >= 32;
}

bool c6_bundle_key_in(char const *dir, char *out, size_t cap, unsigned *files) {
    char          m[3][40];
    unsigned long sz[3];
    unsigned      present = 0;
    bool          all     = true;
    for (int i = 0; i < 3; i++) {
        all = bundle_file(dir, NAMES[i], m[i], &sz[i], &present) && all;
    }
    if (files) {
        *files = present;
    }
    if (!all || !c6_bundle_key(m[0], m[1], m[2], sz[0], sz[1], sz[2], out, cap)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

// The failed record this card bundle would have under the running P4 image
static bool this_record(char const *card_key, char *out, size_t cap) {
    return c6_card_record(card_key, esp_app_get_description()->app_elf_sha256, out, cap);
}

static bool failed_before(char const *record) {
    char         stored[C6_CARD_RECORD_MAX] = "";
    size_t       len                        = sizeof(stored);
    nvs_handle_t nvs;
    if (!record[0] || nvs_open(FW_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    bool have = nvs_get_str(nvs, KEY_FAILED, stored, &len) == ESP_OK;
    nvs_close(nvs);
    return have && strcmp(stored, record) == 0;
}

// record NULL erases it
static void save_record(char const *record) {
    nvs_handle_t nvs;
    if (nvs_open(FW_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "NVS not open: the failed record is not changed");
        return;
    }
    char   cur[C6_CARD_RECORD_MAX];
    size_t len  = sizeof(cur);
    bool   have = nvs_get_str(nvs, KEY_FAILED, cur, &len) == ESP_OK;
    bool   ok   = true;
    if (record && (!have || strcmp(cur, record) != 0)) {
        ok = fw_nvs_ok(TAG, KEY_FAILED, nvs_set_str(nvs, KEY_FAILED, record));
    } else if (!record && have) {
        ok = fw_nvs_erased(TAG, KEY_FAILED, nvs_erase_key(nvs, KEY_FAILED));
    } else {
        nvs_close(nvs);
        return; // nothing changes: no flash wear
    }
    if (ok) {
        fw_nvs_ok(TAG, KEY_FAILED, nvs_commit(nvs));
    }
    nvs_close(nvs);
}

// The three .bin files match their .md5 and the app fits its partition: what the flasher checks, before it is the source
static bool card_bundle_valid(void) {
    c6_binaries_t bins;
    c6_image_dir_use(C6_CARD_DIR);
    bool ok = get_c6_binaries(&bins);
    free_c6_binaries(&bins);
    return ok;
}

static void notice_card(c6_card_t c, bool md5_ok) {
    if (c == C6_CARD_HALF) {
        c6_hooks_notice("C6 update on the SD card ignored", "The bundle in lichen/update/c6 is incomplete");
    } else if (c == C6_CARD_NEW && !md5_ok) {
        c6_hooks_notice("C6 update on the SD card ignored", "A file in lichen/update/c6 does not match its .md5");
    }
}

bool c6_card_begin(void) {
    memset(&s, 0, sizeof(s));
    c6_image_dir_use(NULL);
    if (!sd_mounted() || !sd_begin(CARD_WAIT_MS)) {
        return false;
    }
    int64_t  t0 = esp_timer_get_time();
    char     card_key[C6_BUNDLE_KEY_MAX], storage_key[C6_BUNDLE_KEY_MAX], record[C6_CARD_RECORD_MAX];
    unsigned files = 0;
    c6_bundle_key_in(C6_CARD_DIR, card_key, sizeof(card_key), &files);
    c6_bundle_key_in(C6_IMAGE_DIR, storage_key, sizeof(storage_key), NULL);
    bool      failed = this_record(card_key, record, sizeof(record)) && failed_before(record);
    c6_card_t c      = c6_card_classify(files, card_key, storage_key, failed);
    bool      md5_ok = c == C6_CARD_NEW && card_bundle_valid();
    s.from_card      = c6_card_use(c, md5_ok);
    if (c != C6_CARD_NONE) {
        ESP_LOGI(TAG, "card bundle %s%s: flasher source %s", c6_card_name(c),
                 c == C6_CARD_NEW && !md5_ok ? " (md5 mismatch)" : "", s.from_card ? C6_CARD_DIR : C6_IMAGE_DIR);
    }
    ESP_LOGI(TAG, "card look %u us", (unsigned)(esp_timer_get_time() - t0));
    notice_card(c, md5_ok);
    if (!s.from_card) {
        c6_image_dir_use(NULL);
        sd_end(SD_IO_OK);
        return false;
    }
    c6_image_dir_use(C6_CARD_DIR);
    snprintf(s.key, sizeof(s.key), "%s", card_key);
    s.locked = true;
    return true;
}

void c6_card_end_reading(void) {
    c6_image_dir_use(NULL);
    if (s.locked) {
        sd_end(SD_IO_OK);
        s.locked = false;
    }
}

// src -> dst through buf; false (dst removed) on any read or write error
static bool copy_file(char const *src, char const *dst, uint8_t *buf, int *err) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        *err = errno;
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        *err = 0; // /storage, not the card
        fclose(in);
        return false;
    }
    bool   ok = true;
    size_t n;
    while (ok && (n = fread(buf, 1, COPY_PIECE, in)) > 0) {
        ok = fwrite(buf, 1, n, out) == n;
    }
    bool read_ok = !ferror(in);
    *err         = read_ok ? 0 : errno; // only a card read error counts against the card
    fclose(in);
    ok = fclose(out) == 0 && ok && read_ok;
    if (!ok) {
        unlink(dst);
    }
    return ok;
}

// What landed on /storage matches the .md5 that came with it (read back, not the card's bytes)
static bool landed_ok(char const *bin, char const *md5_file, uint8_t *buf) {
    char  hex[40] = "";
    FILE *f       = fopen(md5_file, "r");
    if (!f) {
        return false;
    }
    size_t m = fread(hex, 1, sizeof(hex) - 1, f);
    fclose(f);
    hex[m] = '\0';
    f      = fopen(bin, "rb");
    if (!f) {
        return false;
    }
    md5_context_t ctx;
    uint8_t       digest[ESP_ROM_MD5_DIGEST_LEN];
    size_t        n;
    esp_rom_md5_init(&ctx);
    while ((n = fread(buf, 1, COPY_PIECE, f)) > 0) {
        esp_rom_md5_update(&ctx, buf, n);
    }
    bool read_ok = !ferror(f);
    fclose(f);
    esp_rom_md5_final(digest, &ctx);
    return read_ok && c6_md5_matches(hex, digest);
}

// The six paths of one file pair: card source, /storage temp and final
typedef struct {
    char src[80], tmp[80], dst[80];
} copy_paths_t;

static bool pair_paths(int i, bool md5, copy_paths_t *p) {
    char name[40];
    snprintf(name, sizeof(name), md5 ? "%s.md5" : "%s", NAMES[i]);
    char tmp_name[48];
    snprintf(tmp_name, sizeof(tmp_name), "%s.new", name);
    return c6_image_path_in(C6_CARD_DIR, name, p->src, sizeof(p->src)) &&
           c6_image_path_in(C6_IMAGE_DIR, tmp_name, p->tmp, sizeof(p->tmp)) &&
           c6_image_path_in(C6_IMAGE_DIR, name, p->dst, sizeof(p->dst));
}

// The six files into temps next to the old bundle, each .bin read back against its .md5, and only when all six landed
// the renames, so a copy that runs out of room (firmware.bin, the largest, comes last) never leaves a mix of old and
// new pairs that each match their own .md5 (#595 review M2). Only a power cut between two renames can still mix them;
// the next boot with the card copies again (the card and /storage keys differ).
static bool copy_all(uint8_t *buf, int *err) {
    copy_paths_t p, md5;
    bool         ok = true;
    mkdir(C6_IMAGE_DIR, 0775); // an existing directory is fine
    for (int k = 0; k < 6 && ok; k++) {
        ok = pair_paths(k / 2, k % 2, &p) && copy_file(p.src, p.tmp, buf, err);
    }
    for (int i = 0; i < 3 && ok; i++) {
        ok = pair_paths(i, false, &p) && pair_paths(i, true, &md5) && landed_ok(p.tmp, md5.tmp, buf);
    }
    for (int k = 0; k < 6 && ok; k++) {
        ok = pair_paths(k / 2, k % 2, &p) && rename(p.tmp, p.dst) == 0;
    }
    for (int k = 0; k < 6; k++) {
        if (pair_paths(k / 2, k % 2, &p)) {
            unlink(p.tmp); // left over after a failure; gone already after the rename
        }
    }
    return ok;
}

// Room for a whole second bundle on /storage (c6_card_copy_fits), from the card's .bin sizes
static bool copy_room(void) {
    unsigned long sizes[3];
    for (int i = 0; i < 3; i++) {
        char        path[80];
        struct stat st;
        if (!c6_image_path_in(C6_CARD_DIR, NAMES[i], path, sizeof(path)) || stat(path, &st) != 0) {
            return false;
        }
        sizes[i] = (unsigned long)st.st_size;
    }
    uint64_t free_b = 0, total_b = 0;
    bool     known  = storage_free_bytes(&free_b, &total_b);
    return c6_card_copy_fits(known, free_b, sizes);
}

// A copy a power cut ended: its temps must not count against the room (review M-a). Own frame: the paths stay off
// c6_card_finish's stack.
__attribute__((noinline)) static void sweep_copy_temps(void) {
    copy_paths_t p;
    for (int k = 0; k < 6; k++) {
        if (pair_paths(k / 2, k % 2, &p)) {
            unlink(p.tmp);
        }
    }
}

typedef enum { COPY_OK, COPY_NO_ROOM, COPY_FAILED } copy_t;

static copy_t copy_to_storage(void) {
    uint8_t *buf = heap_caps_malloc(COPY_PIECE, MALLOC_CAP_SPIRAM); // short-lived; fread and fwrite copy
    if (!buf) {
        return COPY_FAILED;
    }
    copy_t r   = COPY_FAILED;
    int    err = 0;
    sweep_copy_temps();
    if (sd_mounted() && sd_begin(CARD_WAIT_MS)) {
        r = !copy_room() ? COPY_NO_ROOM : copy_all(buf, &err) ? COPY_OK : COPY_FAILED;
        sd_end(r == COPY_FAILED && err ? sd_io_from_errno(err) : SD_IO_OK);
    }
    heap_caps_free(buf);
    return r;
}

bool c6_card_from_card(void) {
    return s.from_card;
}

static void copy_and_report(void) {
    copy_t r = copy_to_storage();
    if (r == COPY_OK) {
        ESP_LOGI(TAG, "card bundle proven by IDENT, copied to " C6_IMAGE_DIR);
        save_record(NULL);
        return;
    }
    ESP_LOGW(TAG, "card bundle proven, but not copied to " C6_IMAGE_DIR " (%s; the next boot tries again)",
             r == COPY_NO_ROOM ? "no room" : "copy failed");
    c6_hooks_notice("C6 update not kept on the badge",
                    r == COPY_NO_ROOM ? "Installed from the SD card, but the badge's storage has no room for the copy "
                                        "used without a card"
                                      : "Installed from the SD card, but the copy used without a card failed");
}

void c6_card_finish(bool on_c6, bool ident_ok) {
    c6_card_end_reading(); // no-op normally: c6link ends the reading before the attach
    char record[C6_CARD_RECORD_MAX];
    bool trial = c6_hooks_image_trial_pending();
    switch (c6_card_after(s.from_card, on_c6, ident_ok, trial)) {
    case C6_CARD_COPY:
        copy_and_report();
        break;
    case C6_CARD_RECORD_FAILED:
        ESP_LOGW(TAG, "card bundle flashed but not proven by IDENT: the next boot uses " C6_IMAGE_DIR);
        if (this_record(s.key, record, sizeof(record))) {
            save_record(record);
        }
        c6_hooks_notice("C6 update from the SD card failed",
                        "The C6 did not confirm it; the next boot goes back to the copy on the badge");
        break;
    case C6_CARD_KEEP:
    default:
        if (s.from_card && ident_ok) {
            ESP_LOGI(TAG, "card bundle proven; copied to " C6_IMAGE_DIR " at the first boot off trial");
            c6_hooks_notice("C6 updated from the SD card",
                            "Keep the card in until the next restart: the copy used without a card is made once this "
                            "version is confirmed"); // review L2
        }
        break;
    }
    s.from_card = false;
}
