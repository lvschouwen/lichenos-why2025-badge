// Apps from the SD card, the spike (#517): "fw elf run <path>" loads a native ELF from /sd into PSRAM, resolves its
// symbols against the generated export table (lichen_exports.c, #519), runs the entry once, prints what it cost and unloads it again. Developer
// mode only: console_register_dev leaves the command, and with it this file's code, out of a release image.
#include "sd_apps.h"

#include "lichen_exports.h"
#include "sd_elf_args.h"
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
    int    fd;
    size_t size;
} file_src_t;

// Plain read(): no stdio buffer (newlib would take it from internal RAM) and nothing left holding the file's bytes.
static int src_read(void *ctx, size_t off, void *buf, size_t len) {
    file_src_t *s = ctx;
    if (lseek(s->fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        return -EIO;
    }
    for (size_t got = 0; got < len;) {
        ssize_t n = read(s->fd, (uint8_t *)buf + got, len - got);
        if (n <= 0) {
            return -EIO;
        }
        got += (size_t)n;
    }
    return 0;
}

static size_t src_size(void *ctx) {
    return ((file_src_t *)ctx)->size;
}

static void src_close(void *ctx) {
    file_src_t *s = ctx;
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

typedef struct {
    size_t psram;
    size_t internal;
} heap_snap_t;

static heap_snap_t snap(void) {
    return (heap_snap_t){heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_INTERNAL)};
}

// Opens the file for the loader; false with errno set. The loader closes it (src_close) on every path once given it.
static bool open_source(char const *path, file_src_t *s) {
    s->fd = open(path, O_RDONLY);
    if (s->fd < 0) {
        return false;
    }
    struct stat st;
    int         err = 0;
    if (fstat(s->fd, &st) != 0) {
        err = errno;
    } else if (!S_ISREG(st.st_mode)) {
        err = EISDIR;
    } else if (st.st_size <= 0) {
        err = ENOENT; // an empty file is no image
    }
    if (err) {
        src_close(s);
        errno = err;
        return false;
    }
    s->size = (size_t)st.st_size;
    return true;
}

// The entry runs on the calling task's stack, which is the console's (the update server's task over Wi-Fi): about 4 KB
// of it is free, shared with everything the app calls through the OS. Until every app has its own task (#523) an app
// must stay small: no big locals, no deep call chains. The spike's hello needs next to nothing.
static void run_elf(char const *path) {
    bool on_sd = sd_elf_on_sd(path); // /storage needs no card
    if (on_sd && !sd_begin(2000)) {
        console_reply("elf run: card not mounted (or busy)");
        return;
    }
    file_src_t file;
    if (!open_source(path, &file)) {
        int err = errno;
        if (on_sd) {
            sd_end(sd_io_from_errno(err));
        }
        console_reply("elf run: cannot open %s (errno %d)", path, err);
        return;
    }
    elf_source_t src = {.size = src_size, .read = src_read, .close = src_close, .ctx = &file};
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
    static bool registered; // main calls this once; a second esp_elf_register_symbol would only answer -EEXIST
    if (registered) {
        return;
    }
    registered = true;
    esp_elf_register_symbol(lichen_exports);
    console_register_dev("elf",
                         "elf run <path>  (a native ELF from /sd or /storage: load, run once, unload; the app runs on "
                         "this task's small stack, so keep it small until #523; #517)",
                         cmd_elf);
}

