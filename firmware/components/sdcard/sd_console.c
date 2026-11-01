// "fw sd" (#60): status, mount, unmount, a small write/read test and the explicit format
#include "sdcard.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "fw_console.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH  SD_LICHEN_DIR "/.sdtest"
#define TEST_BYTES (64 * 1024)
#define TEST_CHUNK 4096

static void reply_status(void) {
    if (sd_begin(2000)) {
        sd_end(SD_IO_OK); // refreshes the free space, here on the console task rather than on LVGL's
    }
    sd_info_t info;
    sd_info(&info);
    char line[160];
    sd_status_line(&info, line, sizeof(line));
    console_reply("%s", line);
}

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

static void cmd_format(char const *word) {
    if (!sd_format_confirmed(word)) {
        console_reply("sd format erases everything on the card: fw sd format " SD_FORMAT_CONFIRM);
        return;
    }
    console_reply("sd formatting...");
    sd_format();
    reply_status();
}

static void cmd_sd(char const *args) {
    if (args[0] == '\0' || strcmp(args, "status") == 0) {
        reply_status();
    } else if (strcmp(args, "mount") == 0) {
        sd_mount();
        reply_status();
    } else if (strcmp(args, "unmount") == 0) {
        sd_unmount();
        reply_status();
    } else if (strcmp(args, "test") == 0) {
        cmd_test();
    } else if (strcmp(args, "format") == 0) {
        cmd_format("");
    } else if (strncmp(args, "format ", 7) == 0) {
        cmd_format(args + 7);
    } else {
        console_reply("sd [status | mount | unmount | test | format " SD_FORMAT_CONFIRM "]");
    }
}

void sd_console_register(void) {
    console_register("sd", "sd [status | mount | unmount | test | format " SD_FORMAT_CONFIRM "] (the SD card at /sd, #60)",
                     cmd_sd);
}
