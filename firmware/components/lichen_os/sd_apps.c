// Apps from the SD card, the spike (#517): "fw elf run <path>" loads a native ELF from /sd into PSRAM, resolves its
// symbols against a small export table, runs the entry once, prints what it cost and unloads it again. Developer
// mode only: console_register_dev leaves the command, and with it this file's code, out of a release image.
#include "sd_apps.h"

#include "sd_elf_args.h"
#include "sdcard.h"

#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "fw_console.h"
#include "lichen_notify.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The spike's export table: just what sdapps/hello needs. The full, generated table is #519.
static struct esp_elfsym const k_exports[] = {
    ESP_ELFSYM_EXPORT(lichen_notify_dnd),
    ESP_ELFSYM_EXPORT(esp_rom_printf),
    {NULL, NULL},
};

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
    if (fstat(s->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        int err = st.st_size > 0 ? EISDIR : ENOENT;
        src_close(s);
        errno = err;
        return false;
    }
    s->size = (size_t)st.st_size;
    return true;
}

static void run_elf(char const *path) {
    if (!sd_begin(2000)) {
        console_reply("elf run: card not mounted (or busy)");
        return;
    }
    file_src_t file;
    if (!open_source(path, &file)) {
        int err = errno;
        sd_end(sd_io_from_errno(err));
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
    sd_end(ret == 0 ? SD_IO_OK : SD_IO_NEUTRAL); // a bad image says nothing about the card
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
    sd_elf_args_t     v = sd_elf_parse(args, path, sizeof(path), &why);
    if (v != SD_ELF_ARGS_OK) {
        console_reply("elf: %s", sd_elf_args_text(v, why));
        return;
    }
    run_elf(path);
}

void sd_apps_console_register(void) {
    esp_elf_register_symbol(k_exports);
    console_register_dev("elf", "elf run <path>  (a native ELF from /sd: load, run once, unload; #517)", cmd_elf);
}

