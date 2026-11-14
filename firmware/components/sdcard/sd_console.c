// "fw sd" (#60): status, mount, unmount, a small write/read test, the explicit format and "cat" (#378)
#include "sd_rules.h"
#include "sd_cat_fmt.h"
#include "sd_path_policy.h"
#include "sdcard.h"
#include "secure_wipe.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_console.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PATH  SD_LICHEN_DIR "/.sdtest"
#define TEST_BYTES (64 * 1024)
#define TEST_CHUNK 4096

static void reply_status(void) {
    if (sd_begin(2000)) {
        sd_end(SD_IO_NEUTRAL); // refreshes the free space, here on the console task rather than on LVGL's; no I/O, so it
                               // says nothing about the card and must not reset its failure streak (#125)
    }
    sd_info_t info;
    sd_info(&info);
    char line[160];
    sd_status_line(&info, line, sizeof(line));
    console_reply("%s", line);
}

// "sd test" and "sd cat" are debug commands (#378): with LICHEN_DEV_COMMANDS off (the release build) neither their
// code nor their usage is in the image, so a release "fw sd" knows only status, mount, unmount and format
#ifdef CONFIG_LICHEN_DEV_COMMANDS
static uint8_t pattern(size_t i) {
    return (uint8_t)(i * 31u + 7u + (i >> 9));
}

static double mb_per_s(size_t bytes, int64_t us) {
    return us > 0 ? (double)bytes / (double)us : 0.0; // bytes per µs = MB/s
}

typedef struct {
    char const *failed; // the step that failed, NULL while all is well
    int         err;    // its errno
    sd_io_t     io;     // what it says about the card
} test_state_t;

static void fail(test_state_t *t, char const *step, int err) {
    if (!t->failed) {
        t->failed = step;
        t->err    = err;
        t->io     = sd_io_from_errno(err);
    }
}

static void test_write(test_state_t *t, uint8_t *buf) {
    FILE *f = fopen(TEST_PATH, "wb");
    if (!f) {
        fail(t, "create", errno);
        return;
    }
    for (size_t off = 0; !t->failed && off < TEST_BYTES; off += TEST_CHUNK) {
        for (size_t i = 0; i < TEST_CHUNK; i++) {
            buf[i] = pattern(off + i);
        }
        if (fwrite(buf, 1, TEST_CHUNK, f) != TEST_CHUNK) {
            fail(t, "write", errno);
        }
    }
    if (!t->failed && (fflush(f) != 0 || fsync(fileno(f)) != 0)) {
        fail(t, "flush", errno);
    }
    if (fclose(f) != 0) {
        fail(t, "close", errno);
    }
}

static void test_read(test_state_t *t, uint8_t *buf) {
    FILE *f = fopen(TEST_PATH, "rb");
    if (!f) {
        fail(t, "open", errno);
        return;
    }
    for (size_t off = 0; !t->failed && off < TEST_BYTES; off += TEST_CHUNK) {
        if (fread(buf, 1, TEST_CHUNK, f) != TEST_CHUNK) {
            fail(t, "read", errno ? errno : EIO); // a short read of a file we just wrote is the card's fault
            break;
        }
        for (size_t i = 0; i < TEST_CHUNK; i++) {
            if (buf[i] != pattern(off + i)) {
                fail(t, "compare", EIO); // the card handed back other data: a failing card
                break;
            }
        }
    }
    if (fclose(f) != 0) {
        fail(t, "close", errno);
    }
}

// Write, fsync, read back and compare TEST_BYTES; the file is removed again. Holds the card's mutex throughout.
static void cmd_test(void) {
    if (!sd_begin(2000)) {
        console_reply("sd test: card not mounted (or busy)");
        return;
    }
    uint8_t *buf = heap_caps_malloc(TEST_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        sd_end(SD_IO_NEUTRAL); // says nothing about the card
        console_reply("sd test: out of memory");
        return;
    }
    test_state_t t  = {.io = SD_IO_OK};
    int64_t      t0 = esp_timer_get_time();
    test_write(&t, buf);
    int64_t t1 = esp_timer_get_time();
    if (!t.failed) {
        test_read(&t, buf);
    }
    int64_t t2 = esp_timer_get_time();
    if (unlink(TEST_PATH) != 0 && t.failed == NULL) {
        fail(&t, "remove", errno);
    }
    heap_caps_free(buf);
    sd_end(t.io);
    if (t.failed) {
        console_reply("sd test failed at %s (errno %d)", t.failed, t.err);
        return;
    }
    console_reply("sd test ok: %u KB written at %.2f MB/s, read back at %.2f MB/s", TEST_BYTES / 1024,
                  mb_per_s(TEST_BYTES, t1 - t0), mb_per_s(TEST_BYTES, t2 - t1));
}

// ---- cat (#378) ---------------------------------------------------------------------------------------------------

// Up to len bytes at off of fd; plain read(), so no stdio buffer is left holding the file's bytes. -1 and errno on error.
static ssize_t read_at(int fd, uint32_t off, uint8_t *buf, size_t len) {
    if (lseek(fd, (off_t)off, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break; // the end of the file
        }
        got += (size_t)n;
    }
    return (ssize_t)got;
}

// Reads up to a->len bytes at a->offset of a regular file into buf; *got the bytes read, *size the file's size. NULL
// on success, else what went wrong (errno in *err). The caller holds the card for a path on /sd.
static char const *cat_read(sd_cat_args_t const *a, uint8_t *buf, size_t *got, long *size, int *err) {
    struct stat st;
    if (stat(a->path, &st) != 0) {
        *err = errno;
        return "not found";
    }
    if (!S_ISREG(st.st_mode)) {
        return "not a regular file";
    }
    *size  = (long)st.st_size;
    int fd = open(a->path, O_RDONLY);
    if (fd < 0) {
        *err = errno;
        return "can't open";
    }
    ssize_t n = read_at(fd, a->offset, buf, a->len);
    if (n < 0) {
        *err = errno ? errno : EIO;
    }
    close(fd);
    *got = n < 0 ? 0 : (size_t)n;
    return n < 0 ? "read failed" : NULL;
}

// The bytes go out through console_reply_private: to whoever asked, never into the log store (GET /log)
static void cat_print(sd_cat_args_t const *a, uint8_t const *buf, size_t got) {
    char line[SD_CAT_LINE_CAP];
    for (size_t i = 0; i < got;) {
        size_t used = got - i < SD_CAT_HEX_ROW ? got - i : SD_CAT_HEX_ROW;
        if (a->hex) {
            sd_cat_hex_line(a->offset + (uint32_t)i, buf + i, used, line, sizeof(line));
        } else {
            sd_cat_text_line(buf + i, got - i, &used, line, sizeof(line));
        }
        console_reply_private("%s", line);
        i += used;
    }
    secure_wipe(line, sizeof(line));
}

static void cmd_cat(char const *args) {
    sd_cat_args_t a;
    if (!sd_cat_parse(args, &a)) {
        console_reply("sd cat [-x] <path> [offset [len]] (len 1..%d, default %d)", SD_CAT_MAX, SD_CAT_DEFAULT);
        return;
    }
    sd_path_verdict_t v = sd_path_check(a.path);
    if (v != SD_PATH_OK) {
        console_reply("sd cat: %s", sd_path_verdict_text(v)); // the path is not echoed: it may be long or odd
        return;
    }
    bool on_card = strncmp(a.path, SD_BASE "/", sizeof(SD_BASE)) == 0;
    if (on_card && !sd_begin(2000)) {
        console_reply("sd cat: card not mounted (or busy)");
        return;
    }
    uint8_t    *buf  = heap_caps_malloc(SD_CAT_MAX, MALLOC_CAP_SPIRAM);
    size_t      got  = 0;
    long        size = 0;
    int         err  = 0;
    char const *why  = buf ? cat_read(&a, buf, &got, &size, &err) : "out of memory";
    if (on_card) {
        sd_end(buf ? sd_io_from_errno(err) : SD_IO_NEUTRAL); // only a card error counts against the card
    }
    if (why) {
        console_reply("sd cat: %s (errno %d)", why, err);
    } else {
        console_reply("sd cat: %u bytes at %lu of %ld", (unsigned)got, (unsigned long)a.offset, size);
        cat_print(&a, buf, got);
    }
    if (buf) {
        secure_wipe(buf, SD_CAT_MAX);
        heap_caps_free(buf);
    }
}
#endif // CONFIG_LICHEN_DEV_COMMANDS

static void cmd_format(char const *word) {
    if (!sd_format_confirmed(word)) {
        console_reply("sd format erases everything on the card: fw sd format " SD_FORMAT_CONFIRM);
        return;
    }
    console_reply("sd formatting...");
    sd_format();
    reply_status();
}

static void sub_status(char const *rest) {
    (void)rest;
    reply_status();
}

static void sub_mount(char const *rest) {
    (void)rest;
    sd_mount();
    reply_status();
}

static void sub_unmount(char const *rest) {
    (void)rest;
    sd_unmount();
    reply_status();
}

#ifdef CONFIG_LICHEN_DEV_COMMANDS
static void sub_test(char const *rest) {
    (void)rest;
    cmd_test();
}

#define SD_DEV_SUBS " | test | cat [-x] <path> [offset [len]]"
#define SD_DEV_NOTE "; test and cat are debug commands (fw dev)"
#else
#define SD_DEV_SUBS ""
#define SD_DEV_NOTE ""
#endif
#define SD_USAGE "sd [status | mount | unmount" SD_DEV_SUBS " | format " SD_FORMAT_CONFIRM "]"

// A table rather than an else-if chain (#378 pushed the chain past the nesting line). with_args: the word may be
// followed by " ..." (cat, format), else it must stand alone.
static struct {
    char const *word;
    bool        with_args;
    void (*fn)(char const *rest);
} const SUBS[] = {
    {"", false, sub_status},         {"status", false, sub_status}, {"mount", false, sub_mount},
    {"unmount", false, sub_unmount}, {"format", true, cmd_format},
#ifdef CONFIG_LICHEN_DEV_COMMANDS
    {"test", false, sub_test},       {"cat", true, cmd_cat},
#endif
};

static void cmd_sd(char const *args) {
    for (size_t i = 0; i < sizeof(SUBS) / sizeof(SUBS[0]); i++) {
        size_t n = strlen(SUBS[i].word);
        if (strncmp(args, SUBS[i].word, n) != 0) {
            continue;
        }
        char const *rest = args + n;
        if (*rest == '\0' || (SUBS[i].with_args && n > 0 && *rest == ' ')) {
            SUBS[i].fn(*rest ? rest + 1 : rest);
            return;
        }
    }
    console_reply(SD_USAGE);
}

void sd_console_register(void) {
    // test and cat need developer mode, format the USB console: sd_rules.h, checked before cmd_sd runs (#464)
    console_register_rules("sd", SD_USAGE " (the SD card at /sd, #60" SD_DEV_NOTE ")", cmd_sd, SD_RULES);
}
