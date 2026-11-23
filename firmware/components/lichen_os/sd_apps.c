// Apps from the SD card, the spike (#517): "fw elf run <path>" loads a native ELF from /sd into PSRAM, resolves its
// symbols against the generated export table (lichen_exports.c, #519), runs the entry once, prints what it cost and unloads it again. Developer
// mode only: console_register_dev leaves the command, and with it this file's code, out of a release image.
#include "sd_apps.h"

#include "lichen_exports.h"
#include "sd_elf_args.h"
#include "sd_elf_src.h"
#include "sdcard.h"

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_console.h"

#include <errno.h>
#include <stdbool.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    size_t psram;
    size_t internal;
} heap_snap_t;

static heap_snap_t snap(void) {
    return (heap_snap_t){heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_INTERNAL)};
}

// The entry runs on the calling task's stack, which is the console's (the update server's task over Wi-Fi): about 4 KB
// of it is free, shared with everything the app calls through the OS, so an app run this way must stay small. The loader
// service (sd_app_loader.c, #523) gives an app a task of its own; the app's lichen_sd_app_hooks answers false here (no load).
static void run_elf(char const *path) {
    bool on_sd = sd_elf_on_sd(path); // /storage needs no card
    if (on_sd && !sd_begin(2000)) {
        console_reply("elf run: card not mounted (or busy)");
        return;
    }
    sd_elf_file_t file;
    if (!sd_elf_open_source(path, &file)) {
        int err = errno;
        if (on_sd) {
            sd_end(sd_io_from_errno(err));
        }
        console_reply("elf run: cannot open %s (errno %d)", path, err);
        return;
    }
    elf_source_t src = sd_elf_source(&file);
    esp_elf_t    elf;
    heap_snap_t  before = snap();
    int64_t      t0     = esp_timer_get_time();
    esp_elf_init(&elf);
    int ret = esp_elf_relocate_source(&elf, &src); // closes the file
    int64_t t1 = esp_timer_get_time();
    if (on_sd) {
        sd_end(ret == 0 ? SD_IO_OK : SD_IO_NEUTRAL); // a bad image says nothing about the card
    }
    if (ret != 0) {
        esp_elf_deinit(&elf);
        console_reply("elf run: load failed (%d)", ret);
        return;
    }
    heap_snap_t loaded = snap();
    // esp_elf_request() drops the entry's return value; call it directly. hello has no init_array to miss.
    int val = elf.entry ? elf.entry(0, NULL) : -1;
    int64_t t2 = esp_timer_get_time();
    esp_elf_deinit(&elf);
    heap_snap_t after = snap();
    console_reply("elf run: ret=%d load=%" PRId64 " us run=%" PRId64 " us size=%u B", val, t1 - t0, t2 - t1,
                  (unsigned)file.size);
    console_reply("elf run: psram used %d B, internal delta %d B; after unload psram %d B, internal %d B",
                  (int)(before.psram - loaded.psram), (int)(before.internal - loaded.internal),
                  (int)(before.psram - after.psram), (int)(before.internal - after.internal));
}

static void cmd_elf(char const *args) {
    char              path[SD_PATH_MAX + 2];
    sd_path_verdict_t why;
    sd_elf_args_t     v = sd_elf_parse(args, console_usb_hold_why(), path, sizeof(path), &why);
    if (v != SD_ELF_ARGS_OK) {
        char const *hold = v == SD_ELF_ARGS_HELD ? console_usb_hold_why() : NULL; // may have just cleared: then no tail
        console_reply("elf: %s%s%s", sd_elf_args_text(v, why), hold ? ": " : "", hold ? hold : "");
        return;
    }
    run_elf(path);
}

void sd_apps_console_register(void) {
    sd_apps_register_symbols(); // the export table, shared with the loader service (#523)
    console_register_dev("elf",
                         "elf run <path>  (a native ELF from /sd or /storage: load, run once, unload; the app runs on "
                         "this task's small stack, so keep it small until #523; #517)",
                         cmd_elf);
}

